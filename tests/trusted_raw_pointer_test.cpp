#include "oreslang/runtime/TrustedRawPointer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using oreslang::runtime::TrustedExclusiveRawMemoryAuthority;
using oreslang::runtime::TrustedRawAddressGrant;
using oreslang::runtime::TrustedRawPointer;
using oreslang::runtime::TrustedRawProvenance;
using oreslang::runtime::TrustedUnsafeMemoryAuthority;

namespace {
struct NoDefault final {
    int value;
    NoDefault() = delete;
    explicit NoDefault(int input) noexcept : value(input) {}
};
static_assert(std::is_trivially_copyable_v<NoDefault>);

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
        const auto exclusive = TrustedExclusiveRawMemoryAuthority::compiler_trusted();
        const auto provenance = TrustedRawProvenance::compiler_trusted(0xA11CEu, 7, 1);

        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawProvenance::compiler_trusted(0, 0, 0); },
            "zero provenance identity must reject");

        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawProvenance::compiler_trusted(0xA11CEu, 0, 1); },
            "zero provenance generation must reject");
        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawProvenance::compiler_trusted(0xA11CEu, 1, 0); },
            "zero provenance allocation id must reject");

        std::array<std::int32_t, 4> values{{10, 20, 30, 40}};
        const auto values_address = reinterpret_cast<std::uintptr_t>(values.data());
        const auto values_read_grant = TrustedRawAddressGrant::compiler_trusted(
            values_address, sizeof(values), 0, false, 1);
        const auto values_write_grant = TrustedRawAddressGrant::compiler_trusted(
            values_address, sizeof(values), 0, true, 1);
        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawAddressGrant::compiler_trusted(0, 4, 0, false, 1); },
            "zero grant base must reject");
        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawAddressGrant::compiler_trusted(values_address, 0, 0, false, 1); },
            "zero grant extent must reject");
        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawAddressGrant::compiler_trusted(values_address, sizeof(values), 0, false, 0); },
            "zero grant generation must reject");

        auto readonly = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size(), authority, provenance);
        require(readonly.has_provenance(), "safe-derived span must carry explicit provenance");
        require(readonly.load() == 10, "base load");
        require(!readonly.writable(), "ordinary raw formation must be read-only");
        require_throws<std::logic_error>(
            [&] { readonly.store(99); },
            "read-only raw pointer must reject store");

        auto base = TrustedRawPointer<std::int32_t>::from_mut_span(
            values.data(), values.size(), authority, exclusive, provenance);
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
            other_values.data(), other_values.size(), authority,
            TrustedRawProvenance::compiler_trusted(0xB0Bu, 1, 1));
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(other); },
            "unrelated provenance subtraction must reject");

        auto other_address_space = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size(), authority, provenance, 7);
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(other_address_space); },
            "cross-address-space subtraction must reject");

        auto stale_generation = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size(), authority,
            TrustedRawProvenance::compiler_trusted(0xA11CEu, 8, 1));
        require(stale_generation.address() == base.address(),
                "generation test must use the same numeric address");
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(stale_generation); },
            "same region/address with a stale generation must reject");

        auto different_allocation = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size(), authority,
            TrustedRawProvenance::compiler_trusted(0xA11CEu, 7, 2));
        require(different_allocation.address() == base.address(),
                "allocation identity test must use the same numeric address");
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(different_allocation); },
            "same region/generation/address with a different allocation id must reject");

        auto shorter_bounds = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size() - 1, authority, provenance);
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(shorter_bounds); },
            "same provenance with incompatible original bounds must reject");

        auto raw = TrustedRawPointer<std::int32_t>::from_address(
            values_address, authority, values_read_grant);
        auto raw_next = raw.offset_elements(1);
        require(raw.bounded() && raw.address_space() == values_read_grant.address_space(),
                "grant-backed address must be range-bounded in the granted address space");
        require(!raw.has_provenance() && !raw_next.has_provenance(),
                "grant-backed integer pointers must not gain allocation provenance");
        require(!raw_next.writable(), "read-only grant arithmetic must remain read-only");
        require(raw_next.load() == 20, "grant-backed pointer element offset");
        require_throws<std::invalid_argument>(
            [&] { (void)raw_next.distance_elements(raw); },
            "grant-backed pointer subtraction must reject without allocation provenance");
        auto independently_fabricated = TrustedRawPointer<std::int32_t>::from_address(
            values_address, authority, values_read_grant);
        require_throws<std::invalid_argument>(
            [&] { (void)independently_fabricated.distance_elements(raw); },
            "same numeric address/grant must not manufacture allocation provenance");
        require_throws<std::logic_error>(
            [&] {
                (void)TrustedRawPointer<std::int32_t>::from_mut_address(
                    values_address, authority, exclusive, values_read_grant);
            },
            "read-only address grant must not mint mutable authority");
        auto granted_mut = TrustedRawPointer<std::int32_t>::from_mut_address(
            values_address, authority, exclusive, values_write_grant);
        granted_mut.offset_elements(1).store(21);
        require(values[1] == 21, "writable grant must permit bounded mutable access");
        require_throws<std::out_of_range>(
            [&] { (void)granted_mut.offset_elements(4).load(); },
            "grant one-past pointer must not dereference");

        volatile std::int32_t mmio_cell = 9;
        const auto mmio_address =
            reinterpret_cast<std::uintptr_t>(const_cast<std::int32_t*>(&mmio_cell));
        const auto mmio_grant = TrustedRawAddressGrant::compiler_trusted(
            mmio_address, sizeof(mmio_cell), 7, true, 1);
        auto mmio = TrustedRawPointer<std::int32_t>::from_mut_address(
            mmio_address, authority, exclusive, mmio_grant);
        require(mmio.address_space() == 7, "MMIO pointer must inherit grant address space");
        require(mmio.load_volatile() == 9, "volatile raw load");
        mmio.store_volatile(11);
        require(mmio_cell == 11, "volatile raw store");

        require_throws<std::invalid_argument>(
            [&] {
                (void)TrustedRawPointer<std::int32_t>::from_address(
                    0, authority, values_read_grant);
            },
            "null raw address must reject");
        require_throws<std::invalid_argument>(
            [&] {
                (void)TrustedRawPointer<std::int32_t>::from_address(
                    values_address + 1u, authority, values_read_grant);
            },
            "misaligned raw address must reject");
        require_throws<std::out_of_range>(
            [&] {
                (void)TrustedRawPointer<std::int32_t>::from_address(
                    values_address + sizeof(values), authority, values_read_grant);
            },
            "one-past address cannot be used as initial typed mapping");
        require_throws<std::overflow_error>(
            [&] {
                const auto near_max = std::numeric_limits<std::uintptr_t>::max()
                    - static_cast<std::uintptr_t>(3);
                (void)TrustedRawAddressGrant::compiler_trusted(
                    near_max, 8, 0, false, 1);
            },
            "grant extent overflow must reject");

        require_throws<std::out_of_range>(
            [&] {
                (void)raw.offset_elements(
                    std::numeric_limits<std::ptrdiff_t>::min());
            },
            "PTRDIFF_MIN grant arithmetic must reject without signed overflow");

        NoDefault non_default(1234);
        auto non_default_raw = TrustedRawPointer<NoDefault>::from_span(
            &non_default, 1, authority,
            TrustedRawProvenance::compiler_trusted(0xD3FA017u, 1, 1));
        require(non_default_raw.load().value == 1234,
                "trivially-copyable non-default-constructible load must work");

        std::cout << "trusted raw pointer arithmetic/provenance contract: PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "trusted raw pointer contract: " << failure.what() << '\n';
        return 1;
    }
}
