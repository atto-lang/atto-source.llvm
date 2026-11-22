#include "oreslang/frontend/NativeTelemetryLiteralV1.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

static bool independent_oracle(std::string_view value, std::size_t max) {
    if (value.empty() || value.size() > max) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 32 || c == 127) return false;
        if (c == 0xc2 && i + 1 < value.size()) {
            const auto next = static_cast<unsigned char>(value[i + 1]);
            if (next >= 0x80 && next <= 0x9f) return false;
        }
    }
    return true;
}

int main() {
    using oreslang::telemetry::valid_native_log_literal;
    using oreslang::telemetry::valid_native_span_literal;
    assert(valid_native_log_literal("hammer.started"));
    assert(valid_native_span_literal("hammer"));
    assert(valid_native_log_literal(std::string(96, 'x')));
    assert(!valid_native_log_literal(std::string(97, 'x')));
    assert(valid_native_span_literal(std::string(256, 'x')));
    assert(!valid_native_span_literal(std::string(257, 'x')));
    assert(!valid_native_log_literal(""));
    assert(!valid_native_span_literal(""));
    assert(!valid_native_span_literal("line\nname"));
    assert(!valid_native_log_literal("line\tname"));
    assert(!valid_native_log_literal(std::string("nul\0inside", 10)));
    assert(!valid_native_log_literal("del\x7f"));
    // UTF-8 C1 controls include U+0085 (NEL); they are not ASCII bytes.
    assert(!valid_native_log_literal("\xC2\x85"));
    assert(!valid_native_span_literal("before\xC2\x9F" "after"));
    assert(valid_native_log_literal("before\xC2\xA0" "after"));
    assert(valid_native_span_literal("emoji:\xF0\x9F\xA6\x99"));

    std::uint32_t state = 0x9e3779b9u;
    for (int iteration = 0; iteration < 10000; ++iteration) {
        std::string bytes;
        const auto n = static_cast<std::size_t>(state % 300u);
        for (std::size_t i = 0; i < n; ++i) {
            state = state * 1664525u + 1013904223u;
            bytes.push_back(static_cast<char>(state >> 24));
        }
        assert(valid_native_log_literal(bytes) ==
               independent_oracle(bytes, 96));
        assert(valid_native_span_literal(bytes) ==
               independent_oracle(bytes, 256));
    }
    std::cout << "LLVM_NATIVE_TELEMETRY_LITERAL_HOST_PASS:10000" << std::endl;
}
