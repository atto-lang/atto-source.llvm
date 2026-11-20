#pragma once
// Independent native frontend's cold-path validation for literal telemetry.
// The parser supplies decoded UTF-8 bytes; no actor heap allocation, OS
// clock, runtime trace context or LLVM SDK dependency is involved here.
#include <cstddef>
#include <string_view>

namespace oreslang::telemetry {

inline bool has_telemetry_literal_control(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte < 32 || byte == 127) return true;
        // UTF-8 U+0080..U+009F: ISO C1 controls, including U+0085 NEL.
        // Mirrors Character.isISOControl in the independent Java backend.
        if (byte == 0xc2 && i + 1 < value.size()) {
            const auto next = static_cast<unsigned char>(value[i + 1]);
            if (next >= 0x80 && next <= 0x9f) return true;
        }
    }
    return false;
}

inline bool valid_native_log_literal(std::string_view value) {
    return !value.empty() && value.size() <= 96 &&
        !has_telemetry_literal_control(value);
}

inline bool valid_native_span_literal(std::string_view value) {
    return !value.empty() && value.size() <= 256 &&
        !has_telemetry_literal_control(value);
}

} // namespace oreslang::telemetry
