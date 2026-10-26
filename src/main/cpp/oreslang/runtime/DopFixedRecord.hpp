#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace oreslang::dop {

namespace detail {

template<class T>
inline constexpr bool fixed_record_member_v =
        std::is_same_v<std::remove_cv_t<T>, std::int32_t>
        || std::is_same_v<std::remove_cv_t<T>, std::int64_t>
        || std::is_same_v<std::remove_cv_t<T>, float>
        || std::is_same_v<std::remove_cv_t<T>, double>
        || std::is_same_v<std::remove_cv_t<T>, bool>;

template<class T>
consteval std::size_t fixed_record_member_width() {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) return 1;
    else if constexpr (std::is_same_v<U, std::int32_t> || std::is_same_v<U, float>) return 4;
    else if constexpr (std::is_same_v<U, std::int64_t> || std::is_same_v<U, double>) return 8;
    else return 0;
}

template<class UInt>
inline void store_unsigned_le(std::uint8_t* out, UInt bits) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        out[i] = static_cast<std::uint8_t>((bits >> (i * 8U)) & UInt{0xff});
    }
}

template<class UInt>
inline UInt load_unsigned_le(const std::uint8_t* in) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    UInt bits = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        bits |= static_cast<UInt>(in[i]) << (i * 8U);
    }
    return bits;
}

template<class T>
inline void encode_fixed_member(std::uint8_t* out, T value) noexcept {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        out[0] = value ? 1U : 0U;
    } else if constexpr (std::is_same_v<U, std::int32_t>) {
        store_unsigned_le(out, std::bit_cast<std::uint32_t>(value));
    } else if constexpr (std::is_same_v<U, std::int64_t>) {
        store_unsigned_le(out, std::bit_cast<std::uint64_t>(value));
    } else if constexpr (std::is_same_v<U, float>) {
        store_unsigned_le(out, std::bit_cast<std::uint32_t>(value));
    } else if constexpr (std::is_same_v<U, double>) {
        store_unsigned_le(out, std::bit_cast<std::uint64_t>(value));
    }
}

template<class T>
inline T decode_fixed_member(const std::uint8_t* in) {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        if (in[0] > 1U) {
            throw std::invalid_argument("noncanonical boolean byte in fixed inverted record");
        }
        return in[0] == 1U;
    } else if constexpr (std::is_same_v<U, std::int32_t>) {
        return std::bit_cast<std::int32_t>(load_unsigned_le<std::uint32_t>(in));
    } else if constexpr (std::is_same_v<U, std::int64_t>) {
        return std::bit_cast<std::int64_t>(load_unsigned_le<std::uint64_t>(in));
    } else if constexpr (std::is_same_v<U, float>) {
        return std::bit_cast<float>(load_unsigned_le<std::uint32_t>(in));
    } else {
        static_assert(std::is_same_v<U, double>);
        return std::bit_cast<double>(load_unsigned_le<std::uint64_t>(in));
    }
}

template<class T>
struct is_fixed_record : std::false_type { };

template<class T>
inline constexpr bool is_fixed_record_v = is_fixed_record<std::remove_cv_t<T>>::value;

} // namespace detail

/**
 * One opaque AoS value stored as a single cell inside an inverted-field column.
 *
 * Members are encoded packed little-endian with canonical bool bytes (0/1).
 * The compiler must instantiate Members in canonical structural member-name
 * order. Nested records and variable-sized/resource-owning members are rejected.
 *
 * Encoded bytes are returned only as detached copies. Mutable column loans are
 * disallowed for FixedRecord fields so callers cannot create noncanonical bool
 * encodings behind the validated value boundary.
 */
template<class... Members>
class FixedRecord final {
    static_assert(sizeof...(Members) != 0,
                  "fixed inverted records require at least one member");
    static_assert(sizeof...(Members) <= 1024,
                  "fixed inverted record exceeds host-reference member limit");
    static_assert((detail::fixed_record_member_v<Members> && ...),
                  "fixed record members must be i32/i64/f32/f64/bool");

    using MemberTuple = std::tuple<Members...>;

    template<std::size_t I>
    static consteval std::size_t offset_of() {
        static_assert(I < sizeof...(Members));
        constexpr std::array<std::size_t, sizeof...(Members)> widths{
            detail::fixed_record_member_width<Members>()...
        };
        std::size_t offset = 0;
        for (std::size_t i = 0; i < I; ++i) offset += widths[i];
        return offset;
    }

public:
    static constexpr std::size_t encoded_size =
            (detail::fixed_record_member_width<Members>() + ... + std::size_t{0});
    static_assert(encoded_size <= 64U * 1024U,
                  "fixed inverted record exceeds host-reference byte limit");

private:
    std::array<std::uint8_t, encoded_size> bytes_{};

    template<std::size_t... I>
    static void validate(const FixedRecord& value, std::index_sequence<I...>) {
        ((void)value.template get<I>(), ...);
    }

public:
    FixedRecord() = default;
    FixedRecord(const FixedRecord&) = default;
    FixedRecord& operator=(const FixedRecord&) = default;
    ~FixedRecord() = default;

    static FixedRecord pack(Members... values) noexcept {
        FixedRecord record;
        std::size_t offset = 0;
        ((detail::encode_fixed_member(record.bytes_.data() + offset, values),
          offset += detail::fixed_record_member_width<Members>()), ...);
        return record;
    }

    static FixedRecord from_bytes(std::span<const std::uint8_t> encoded) {
        if (encoded.size() != encoded_size) {
            throw std::invalid_argument("fixed inverted record payload width mismatch");
        }
        FixedRecord record;
        for (std::size_t i = 0; i < encoded_size; ++i) record.bytes_[i] = encoded[i];
        validate(record, std::index_sequence_for<Members...>{});
        return record;
    }

    template<std::size_t I>
    auto get() const -> std::tuple_element_t<I, MemberTuple> {
        static_assert(I < sizeof...(Members));
        using T = std::tuple_element_t<I, MemberTuple>;
        return detail::decode_fixed_member<T>(bytes_.data() + offset_of<I>());
    }

    std::array<std::uint8_t, encoded_size> bytes() const noexcept {
        return bytes_;
    }

    bool operator==(const FixedRecord&) const = default;
};

namespace detail {

template<class... Members>
struct is_fixed_record<FixedRecord<Members...>> : std::true_type { };

} // namespace detail

} // namespace oreslang::dop
