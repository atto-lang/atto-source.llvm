#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace atto::shape {

// Bounded, allocation-free compiler-field witness for a single readonly
// public int64 field. No C++ RTTI, reflection, dynamic methods or pointer
// reinterpretation. Target native lowering is NOT wired to this witness yet.
enum class Scalar : std::uint8_t { Int, Bool, String, Float };
enum class Error : std::uint8_t {
    None, MissingField, WrongType, PrivateField, StateBorrow, Bounds,
    Moved, DuplicateField, InvalidLayout
};
struct Field {
    std::string_view name;
    Scalar type;
    std::size_t offset;
    bool public_visible;
    bool actor_state;
    bool plain_data_field;
};
struct Layout {
    std::string_view nominal_name;
    std::span<const Field> fields;
    bool sealed_actor_state;
};
struct OwnerLease {
    bool moved{false};
};
struct Value {
    Error error{Error::None};
    std::int64_t value{0};
    [[nodiscard]] constexpr bool ok() const noexcept { return error == Error::None; }
};

// This witness applies to compiler-owned class layouts, never to host objects.
// Layout offset is checked against the owner allocation before any read.
// In a hard-realtime backend, this is bounded by <=32 compile-known fields.
[[nodiscard]] inline Value read_int(
        const Layout& layout,
        std::string_view required_name,
        std::span<const std::byte> memory,
        const OwnerLease& owner) noexcept {
    if (owner.moved) return {Error::Moved, 0};
    if (layout.sealed_actor_state) return {Error::StateBorrow, 0};
    if (layout.nominal_name.empty() || required_name.empty() ||
        layout.fields.size() > 32) return {Error::InvalidLayout, 0};
    const Field* match = nullptr;
    for (std::size_t i = 0; i < layout.fields.size(); ++i) {
        const auto& f = layout.fields[i];
        if (f.name.empty() || (f.offset % 8) != 0) return {Error::InvalidLayout, 0};
        for (std::size_t j = i + 1; j < layout.fields.size(); ++j) {
            if (layout.fields[j].name == f.name) return {Error::DuplicateField, 0};
            // All supported slots in this bounded witness are aligned to 8.
            // Differently named fields must not claim the same physical slot.
            if (layout.fields[j].offset == f.offset)
                return {Error::InvalidLayout, 0};
        }
        if (f.name == required_name) match = &f;
    }
    if (match == nullptr) return {Error::MissingField, 0};
    if (!match->public_visible || match->actor_state || !match->plain_data_field) {
        return {Error::PrivateField, 0};
    }
    if (match->type != Scalar::Int) return {Error::WrongType, 0};
    if (memory.size() < 8 || match->offset > memory.size() - 8) {
        return {Error::Bounds, 0};
    }
    // Explicit LE decoding is valid even on big-endian host targets. Never
    // dereference an unaligned/foreign native pointer as a different class.
    std::uint64_t bits = 0;
    for (unsigned i = 0; i != 8; ++i) {
        bits |= std::uint64_t(std::to_integer<std::uint8_t>(memory[match->offset + i])) << (8 * i);
    }
    std::int64_t signed_value;
    std::memcpy(&signed_value, &bits, sizeof(signed_value));
    return {Error::None, signed_value};
}

}  // namespace atto::shape
