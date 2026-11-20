#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oreslang::runtime {

/**
 * Allocation-free W3C traceparent v00 carrier for native AOT/embedded ABI.
 * This contains IDs/flags only. It is not the existing OresNativeExecutionId,
 * a mutable Span, an entropy provider, or an OTLP exporter.
 */
struct NativeTraceParentV00 final {
    std::array<std::uint8_t, 16> trace_id{};
    std::array<std::uint8_t, 8> span_id{};
    std::uint8_t flags = 0;

    [[nodiscard]] bool sampled() const noexcept {
        return (flags & 1u) != 0;
    }
};

[[nodiscard]] constexpr int trace_hex_nibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

template <std::size_t N>
[[nodiscard]] constexpr bool trace_all_zero(
        const std::array<std::uint8_t, N>& bytes) noexcept {
    for (std::uint8_t byte : bytes) {
        if (byte != 0) return false;
    }
    return true;
}

template <std::size_t N>
[[nodiscard]] inline bool trace_read_hex(
        std::string_view header,
        std::size_t begin,
        std::array<std::uint8_t, N>& output) noexcept {
    if (header.size() < begin + N * 2) return false;
    for (std::size_t i = 0; i < N; i++) {
        const int upper = trace_hex_nibble(header[begin + i * 2]);
        const int lower = trace_hex_nibble(header[begin + i * 2 + 1]);
        if (upper < 0 || lower < 0) return false;
        output[i] = static_cast<std::uint8_t>((upper << 4) | lower);
    }
    return true;
}

/**
 * Fail-closed fixed-length v00 admission. The caller must additionally
 * reject duplicate header fields and check origin/tenant trust and optional
 * tracestate syntax. Invalid input never modifies an existing output value.
 */
[[nodiscard]] inline bool parse_traceparent_v00(
        std::string_view header,
        NativeTraceParentV00& output) noexcept {
    if (header.size() != 55 || header[0] != '0' || header[1] != '0'
            || header[2] != '-' || header[35] != '-'
            || header[52] != '-') {
        return false;
    }
    NativeTraceParentV00 candidate{};
    if (!trace_read_hex(header, 3, candidate.trace_id)
            || !trace_read_hex(header, 36, candidate.span_id)
            || trace_all_zero(candidate.trace_id)
            || trace_all_zero(candidate.span_id)) {
        return false;
    }
    int upper = trace_hex_nibble(header[53]);
    int lower = trace_hex_nibble(header[54]);
    if (upper < 0 || lower < 0) return false;
    candidate.flags = static_cast<std::uint8_t>((upper << 4) | lower);
    output = candidate;
    return true;
}

/**
 * Fixed output array, 55 octets without NUL. The header is valid only for a
 * context with nonzero trace/span IDs. Check that before calling this helper.
 */
[[nodiscard]] inline std::array<char, 55> format_traceparent_v00(
        const NativeTraceParentV00& context) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 55> output{};
    output[0] = '0'; output[1] = '0'; output[2] = '-';
    for (std::size_t i = 0; i < context.trace_id.size(); ++i) {
        const std::uint8_t value = context.trace_id[i];
        output[3 + 2 * i] = digits[value >> 4];
        output[3 + 2 * i + 1] = digits[value & 15];
    }
    output[35] = '-';
    for (std::size_t i = 0; i < context.span_id.size(); ++i) {
        const std::uint8_t value = context.span_id[i];
        output[36 + 2 * i] = digits[value >> 4];
        output[36 + 2 * i + 1] = digits[value & 15];
    }
    output[52] = '-';
    output[53] = digits[context.flags >> 4];
    output[54] = digits[context.flags & 15];
    return output;
}

[[nodiscard]] inline bool trace_child_context(
        const NativeTraceParentV00& parent,
        const std::array<std::uint8_t, 8>& securely_minted_child_span_id,
        NativeTraceParentV00& output) noexcept {
    if (trace_all_zero(parent.trace_id) || trace_all_zero(parent.span_id)
            || trace_all_zero(securely_minted_child_span_id)
            || securely_minted_child_span_id == parent.span_id) {
        return false;
    }
    output = parent;
    output.span_id = securely_minted_child_span_id;
    return true;
}

} // namespace oreslang::runtime
