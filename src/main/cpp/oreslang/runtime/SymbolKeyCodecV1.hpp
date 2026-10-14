#pragma once

// Host-only portable Symbol property-key envelope. Not wired to LLVM guest
// codegen/actor transport. Non-ASCII Symbol names are rejected until a real
// Unicode NFC normalizer is integrated, rather than admitting unverified IDs.

#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef ORES_SYMBOL_V1_ICU_NFC
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>
#include <unicode/uversion.h>
#endif

namespace oreslang::runtime::symbol_key_v1 {

enum class Kind : std::uint8_t { Text = 0, Process = 1, Stable = 2 };

// Keep authorization failures distinct from malformed wire input. Java's
// SecurityException is not an invalid-argument/codec failure either.
class AdmissionDenied final : public std::runtime_error {
public:
    explicit AdmissionDenied(const char* reason) : std::runtime_error(reason) {}
};
struct Key {
    Kind kind;
    std::string name;
    bool operator!=(const Key& rhs) const noexcept { return !(*this == rhs); }
    bool operator<(const Key& rhs) const noexcept {
        return kind != rhs.kind ? kind < rhs.kind : name < rhs.name;
    }
    bool operator==(const Key& rhs) const noexcept {
        return kind == rhs.kind && name == rhs.name;
    }
};

inline constexpr std::size_t kTextBytes = 16'384;
inline constexpr std::size_t kSymbolBytes = 256;
inline constexpr std::size_t kHeaderBytes = 6;
inline constexpr std::size_t kMaxKeys = 4'096;
inline constexpr std::size_t kAggregateWireBytes = 1'048'576;

inline bool validUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto a = static_cast<unsigned char>(text[i]);
        if (a <= 0x7f) { ++i; continue; }
        std::uint32_t point = 0;
        std::size_t width = 0;
        std::uint32_t minimum = 0;
        if ((a & 0xe0u) == 0xc0u) { width = 2; point = a & 0x1fu; minimum = 0x80; }
        else if ((a & 0xf0u) == 0xe0u) { width = 3; point = a & 0x0fu; minimum = 0x800; }
        else if ((a & 0xf8u) == 0xf0u) { width = 4; point = a & 0x07u; minimum = 0x10000; }
        else return false;
        if (width > text.size() - i) return false;
        for (std::size_t j = 1; j < width; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0u) != 0x80u) return false;
            point = (point << 6u) | (next & 0x3fu);
        }
        if (point < minimum || point > 0x10ffffu
                || (point >= 0xd800u && point <= 0xdfffu)) return false;
        i += width;
    }
    return true;
}

inline bool canonicalUuid(std::string_view text) noexcept {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
        } else if (!((text[i] >= '0' && text[i] <= '9')
                || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return true;
}

// A pinned Unicode 16.0 data version matches the intended Java 25 Symbol
// target. Do not silently accept code points with a different ICU table
// version. When ICU is unavailable, the codec remains ASCII-only.
inline void validateSymbolUnicode(std::string_view name) {
    if (name.empty() || name.size() > kSymbolBytes || !validUtf8(name))
        throw std::invalid_argument("Symbol requires 1..256 valid UTF-8 bytes");

    // ASCII Symbols are independent of Unicode data versions. Even if an
    // ICU deployment is missing or has unexpected tables, ASCII remains
    // available, while non-ASCII identities fail closed.
    bool ascii = true;
    for (unsigned char cp : name) {
        if (cp >= 0x80u) {
            ascii = false;
            break;
        }
        if (cp < 0x20u || cp == 0x7fu)
            throw std::invalid_argument("control character in Symbol");
    }
    if (ascii) return;
#ifdef ORES_SYMBOL_V1_ICU_NFC
    UVersionInfo version{};
    u_getUnicodeVersion(version);
    if (version[0] != 16 || version[1] != 0)
        throw std::invalid_argument(
            "native Symbol NFC requires pinned Unicode 16.0 data tables");

    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* nfc = unorm2_getNFCInstance(&status);
    if (U_FAILURE(status) || nfc == nullptr)
        throw std::runtime_error("native Unicode NFC normalizer unavailable");

    // UTF-16 units <= UTF-8 bytes for valid Unicode. No unbounded ICU
    // conversions or caller-owned output buffers at this boundary.
    std::vector<UChar> utf16(name.size() + 1);
    int32_t length = 0;
    status = U_ZERO_ERROR;
    u_strFromUTF8(utf16.data(), static_cast<int32_t>(utf16.size()),
                  &length, name.data(), static_cast<int32_t>(name.size()),
                  &status);
    if (U_FAILURE(status) || length < 0)
        throw std::invalid_argument("invalid Symbol UTF-8 sequence");

    status = U_ZERO_ERROR;
    const UBool isNfc = unorm2_isNormalized(
            nfc, utf16.data(), length, &status);
    if (U_FAILURE(status) || !isNfc)
        throw std::invalid_argument("noncanonical NFC Symbol wire name");

    for (int32_t index = 0; index < length;) {
        UChar32 cp = 0;
        U16_NEXT(utf16.data(), index, length, cp);
        if (cp < 0 || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)
                || u_charType(cp) == U_FORMAT_CHAR
                || cp == 0x061c || cp == 0x200e || cp == 0x200f
                || (cp >= 0x202a && cp <= 0x202e)
                || (cp >= 0x2066 && cp <= 0x2069)) {
            throw std::invalid_argument(
                "control, formatting or bidi character in Symbol");
        }
    }
#else
    throw std::invalid_argument(
        "non-ASCII Symbol requires pinned ICU NFC tables");
#endif
}

inline void validate(const Key& key) {
    switch (key.kind) {
        case Kind::Text:
            if (key.name.size() > kTextBytes || !validUtf8(key.name))
                throw std::invalid_argument("invalid UTF-8 or oversized String key");
            return;
        case Kind::Process:
        case Kind::Stable:
            validateSymbolUnicode(key.name);
            if (key.kind == Kind::Stable && !canonicalUuid(key.name))
                throw std::invalid_argument("stable Symbol requires canonical lowercase UUID");
            return;
    }
    throw std::invalid_argument("unknown property key namespace");
}

inline std::vector<std::uint8_t> encode(const Key& key) {
    validate(key);
    const auto length = key.name.size();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderBytes + length);
    bytes.push_back('O'); bytes.push_back('K'); bytes.push_back(1);
    bytes.push_back(static_cast<std::uint8_t>(key.kind));
    bytes.push_back(static_cast<std::uint8_t>(length >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(length & 0xffu));
    bytes.insert(bytes.end(), key.name.begin(), key.name.end());
    return bytes;
}

inline Key decode(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kHeaderBytes + kTextBytes
            || bytes[0] != 'O' || bytes[1] != 'K' || bytes[2] != 1
            || bytes[3] > static_cast<std::uint8_t>(Kind::Stable))
        throw std::invalid_argument("malformed property-key v1 envelope");
    const std::size_t length = (static_cast<std::size_t>(bytes[4]) << 8u) | bytes[5];
    if (length != bytes.size() - kHeaderBytes)
        throw std::invalid_argument("property-key wire length mismatch");
    Key result{static_cast<Kind>(bytes[3]),
               std::string(bytes.begin() + kHeaderBytes, bytes.end())};
    validate(result);
    return result;
}

// Default untrusted policy. The C++ host-only codec intentionally does not
// accept a caller-supplied export list: receiver-authenticated tenant scopes
// are not wired into native transport yet. For now admit ordinary String
// keys and the sealed public PROCESS core list only.
inline bool publicToUntrustedCore(const Key& key) noexcept {
    return key.kind == Kind::Process
        && (key.name == "ready" || key.name == "error"
            || key.name == "Symbol.iterator");
}

inline Key decodeForUntrusted(const std::vector<std::uint8_t>& bytes) {
    Key key = decode(bytes);
    if (key.kind != Kind::Text && !publicToUntrustedCore(key))
        throw AdmissionDenied("Symbol key is not visible to untrusted actor");
    return key;
}

// Enforce the untrusted policy before inserting keys into the detached
// collection. Trusted and untrusted entry points share exactly the same
// cardinality, wire-byte and logical-equality invariants.
inline std::vector<Key> decodeDistinctChecked(
        const std::vector<std::vector<std::uint8_t>>& frames,
        bool untrusted) {
    if (frames.size() > kMaxKeys)
        throw std::invalid_argument("too many property keys");
    std::size_t total = 0;
    std::set<Key> seen;
    std::vector<Key> keys;
    keys.reserve(frames.size());
    for (const auto& frame : frames) {
        if (frame.size() > kAggregateWireBytes - total)
            throw std::invalid_argument("aggregate property-key wire bytes exceeded");
        total += frame.size();
        Key key = untrusted ? decodeForUntrusted(frame) : decode(frame);
        if (!seen.insert(key).second)
            throw std::invalid_argument("duplicate logical property key");
        keys.push_back(std::move(key));
    }
    return keys;
}

inline std::vector<Key> decodeDistinct(
        const std::vector<std::vector<std::uint8_t>>& frames) {
    return decodeDistinctChecked(frames, false);
}

inline std::vector<Key> decodeDistinctForUntrusted(
        const std::vector<std::vector<std::uint8_t>>& frames) {
    return decodeDistinctChecked(frames, true);
}

} // namespace oreslang::runtime::symbol_key_v1
