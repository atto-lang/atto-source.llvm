#pragma once

// Compiler-owned, static AOT byte/bit pattern plans. No runtime pattern
// interpreter, reflection, dynamic memory, or host-byte-order dependency.
// Guest lowering must supply a checked lifetime/borrow proof for borrowed
// spans; this header is a native host oracle, not that proof.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang {

struct NativePatternByteSpan {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    bool valid() const noexcept { return data != nullptr || size == 0; }
};

struct NativePatternBitSpan {
    NativePatternByteSpan bytes;
    std::size_t offset = 0;
    std::size_t length = 0;

    bool valid() const noexcept {
        if (!bytes.valid() || bytes.size > std::numeric_limits<std::size_t>::max() / 8)
            return false;
        const auto total = bytes.size * 8;
        return offset <= total && length <= total - offset;
    }
};

enum class NativePatternElementKind : std::uint8_t { Equal, Capture, Ignore };

struct NativePatternElement {
    NativePatternElementKind kind;
    std::uint8_t value = 0;

    static constexpr NativePatternElement eq(std::uint8_t literal) noexcept {
        return {NativePatternElementKind::Equal, literal};
    }
    static constexpr NativePatternElement capture() noexcept {
        return {NativePatternElementKind::Capture, 0};
    }
    static constexpr NativePatternElement ignore() noexcept {
        return {NativePatternElementKind::Ignore, 0};
    }
};

template <std::size_t N>
struct NativeBytePatternMatch {
    bool matched = false;
    // Only Capture slots have meaningful data, indexed by source element.
    std::array<std::uint8_t, N> captured{};
    NativePatternByteSpan tail;
};

template <std::size_t N>
NativeBytePatternMatch<N> native_match_bytes(
        NativePatternByteSpan source,
        const std::array<NativePatternElement, N>& pattern,
        bool capture_tail = false) noexcept {
    NativeBytePatternMatch<N> result;
    if (!source.valid() || (capture_tail && N == 0) || N > source.size || (!capture_tail && N != source.size))
        return result;
    for (std::size_t i = 0; i < N; ++i) {
        switch (pattern[i].kind) {
            case NativePatternElementKind::Equal:
                if (source.data[i] != pattern[i].value) return result;
                break;
            case NativePatternElementKind::Capture:
                result.captured[i] = source.data[i];
                break;
            case NativePatternElementKind::Ignore:
                break;
            default:
                return result;
        }
    }
    if (capture_tail) {
        // Arithmetic on null pointers is undefined even when adding zero.
        result.tail = {source.data == nullptr ? nullptr : source.data + N, source.size - N};
    }
    result.matched = true;
    return result;
}

template <std::size_t N>
struct NativeBitPatternMatch {
    bool matched = false;
    std::array<bool, N> captured{};
    // A bit tail may begin at any bit, not necessarily at a byte boundary.
    NativePatternBitSpan tail;
};

template <std::size_t N>
NativeBitPatternMatch<N> native_match_bits(
        NativePatternBitSpan source,
        const std::array<NativePatternElement, N>& pattern,
        bool capture_tail = false) noexcept {
    NativeBitPatternMatch<N> result;
    if (!source.valid() || (capture_tail && N == 0) || N > source.length || (!capture_tail && N != source.length))
        return result;
    for (std::size_t i = 0; i < N; ++i) {
        const auto absolute = source.offset + i;
        const std::uint8_t current = static_cast<std::uint8_t>(
                (source.bytes.data[absolute / 8] >> (7 - (absolute % 8))) & 1u);
        switch (pattern[i].kind) {
            case NativePatternElementKind::Equal:
                if (pattern[i].value > 1 || current != pattern[i].value) return result;
                break;
            case NativePatternElementKind::Capture:
                result.captured[i] = current != 0;
                break;
            case NativePatternElementKind::Ignore:
                break;
            default:
                return result;
        }
    }
    if (capture_tail) {
        result.tail = {source.bytes, source.offset + N, source.length - N};
    }
    result.matched = true;
    return result;
}

// A typed binary {} / packet {} field cursor. A compiler emits calls or
// equivalent inlined fixed-width loads and branches from a STATIC segment
// plan; field order and dependent lengths are validated in typed IR first.
class NativeBinaryPatternCursor {
public:
    explicit NativeBinaryPatternCursor(NativePatternByteSpan source) noexcept
        : source_(source), valid_(source.valid()
                && source.size <= std::numeric_limits<std::size_t>::max() / 8) {}

    // Every typed segment must consume 1..64 bits; a zero-width segment
    // cannot manufacture a successful field or hide an invalid schema.
    bool take_bits(unsigned width, std::uint64_t& value) noexcept {
        if (!valid_ || width == 0 || width > 64 || width > available_bits()) return false;
        std::uint64_t next = 0;
        for (unsigned i = 0; i < width; ++i) {
            const auto pos = bit_offset_ + i;
            next = (next << 1u) | static_cast<std::uint64_t>(
                    (source_.data[pos / 8] >> (7 - (pos % 8))) & 1u);
        }
        bit_offset_ += width;
        value = next;
        return true;
    }

    bool take_uint_bytes(unsigned width, bool little_endian, std::uint64_t& value) noexcept {
        if (width == 0 || width > 64 || width % 8 != 0 || bit_offset_ % 8 != 0)
            return false;
        NativePatternByteSpan slice;
        if (!take_bytes(width / 8, slice)) return false;
        std::uint64_t next = 0;
        for (std::size_t i = 0; i < slice.size; ++i) {
            const auto v = slice.data[little_endian ? slice.size - 1 - i : i];
            next = (next << 8u) | v;
        }
        value = next;
        return true;
    }

    bool take_bytes(std::size_t length, NativePatternByteSpan& view) noexcept {
        if (!valid_ || bit_offset_ % 8 != 0 || length > available_bits() / 8) return false;
        const auto byte_offset = bit_offset_ / 8;
        const NativePatternByteSpan next{
            source_.data == nullptr ? nullptr : source_.data + byte_offset, length};
        bit_offset_ += length * 8;
        view = next;
        return true;
    }

    bool remaining_bytes(NativePatternByteSpan& tail) noexcept {
        return take_bytes(available_bits() / 8, tail) && exact();
    }

    bool exact() const noexcept { return valid_ && bit_offset_ == source_.size * 8; }
    std::size_t bit_offset() const noexcept { return bit_offset_; }

private:
    std::size_t available_bits() const noexcept {
        return valid_ ? source_.size * 8 - bit_offset_ : 0;
    }
    NativePatternByteSpan source_;
    std::size_t bit_offset_ = 0;
    bool valid_ = false;
};

} // namespace oreslang
