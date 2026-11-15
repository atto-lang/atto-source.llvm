#include "oreslang/runtime/NativeTraceParentV00.hpp"

#include <cassert>
#include <array>
#include <string>
#include <string_view>

using oreslang::runtime::NativeTraceParentV00;
using oreslang::runtime::parse_traceparent_v00;
using oreslang::runtime::format_traceparent_v00;
using oreslang::runtime::trace_child_context;
using oreslang::runtime::trace_all_zero;

// Host-only C++ test: not LLVM-compiled Atto source or a network/OTLP proof.
int main() {
    constexpr std::string_view good =
        "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01";
    static_assert(good.size() == 55, "W3C v00 length");
    NativeTraceParentV00 parsed{};
    assert(parse_traceparent_v00(good, parsed));
    assert(parsed.sampled());
    auto encoded = format_traceparent_v00(parsed);
    assert(std::string_view(encoded.data(), encoded.size()) == good);
    assert(!trace_all_zero(parsed.trace_id));
    assert(!trace_all_zero(parsed.span_id));

    for (const auto& invalid : {
        "",
        "ff-0123456789abcdef0123456789abcdef-abcdef0123456789-01",
        "00-00000000000000000000000000000000-abcdef0123456789-01",
        "00-0123456789abcdef0123456789abcdef-0000000000000000-01",
        "00-0123456789ABCDEF0123456789abcdef-abcdef0123456789-01",
        "00-0123456789abcdef0123456789abcdef-abcdef0123456789-0g",
        "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01-extra",
        "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01\r\n"
    }) {
        NativeTraceParentV00 unchanged = parsed;
        assert(!parse_traceparent_v00(invalid, unchanged));
        assert(unchanged.trace_id == parsed.trace_id);
        assert(unchanged.span_id == parsed.span_id);
        assert(unchanged.flags == parsed.flags);
    }

    std::array<std::uint8_t, 8> next_span{};
    next_span[0] = 0x12;
    NativeTraceParentV00 child{};
    assert(trace_child_context(parsed, next_span, child));
    assert(child.trace_id == parsed.trace_id);
    assert(child.span_id == next_span);
    assert(child.flags == parsed.flags);
    auto child_header = format_traceparent_v00(child);
    NativeTraceParentV00 roundtrip{};
    assert(parse_traceparent_v00(
        std::string_view(child_header.data(), child_header.size()), roundtrip));
    assert(roundtrip.trace_id == parsed.trace_id);
    assert(roundtrip.span_id == next_span);

    std::array<std::uint8_t, 8> zero_id{};
    assert(!trace_child_context(parsed, zero_id, child));
    assert(!trace_child_context(parsed, parsed.span_id, child));
    assert(child.span_id == next_span);

    parsed.flags = 0;
    assert(!parsed.sampled());
    const auto unsampled = format_traceparent_v00(parsed);
    NativeTraceParentV00 parsed_unsampled{};
    assert(parse_traceparent_v00(
        std::string_view(unsampled.data(), unsampled.size()), parsed_unsampled));
    assert(!parsed_unsampled.sampled());
    return 0;
}
