#pragma once
#include <cstdint>
#include <string_view>

namespace oreslang::types {
// Java NumericErrorAbi, contract v1: stable language identity, not a host
// exception or process status. Zero and unknown IDs are never valid Errors.
enum class NativeNumericError : std::uint32_t {
    ArithmeticOverflow = 0x00010001u,
    DivisionByZero = 0x00010002u
};
constexpr bool native_numeric_error_valid(std::uint64_t id) noexcept {
    return id == static_cast<std::uint32_t>(NativeNumericError::ArithmeticOverflow) ||
           id == static_cast<std::uint32_t>(NativeNumericError::DivisionByZero);
}
constexpr std::uint32_t native_numeric_error_id(std::string_view name) noexcept {
    if (name == "ArithmeticOverflowError")
        return static_cast<std::uint32_t>(NativeNumericError::ArithmeticOverflow);
    if (name == "DivisionByZeroError")
        return static_cast<std::uint32_t>(NativeNumericError::DivisionByZero);
    return 0;
}
constexpr bool native_numeric_error_type(std::string_view name) noexcept {
    return name == "Error" || native_numeric_error_id(name) != 0;
}
// Unspellable names for the fused unwrap_err() is type projection. These
// never become guest methods, integers, or exposed Error handles.
constexpr std::string_view native_error_projection_prefix = "$error_type:";
constexpr bool native_error_projection(std::string_view name) noexcept {
    return name.substr(0, native_error_projection_prefix.size()) == native_error_projection_prefix;
}
constexpr std::string_view native_error_projection_type(std::string_view name) noexcept {
    return native_error_projection(name) ? name.substr(native_error_projection_prefix.size()) : std::string_view{};
}
} // namespace oreslang::types
