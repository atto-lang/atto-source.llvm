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

namespace oreslang::runtime::symbol_key_v1 {

enum class Kind : std::uint8_t { Text = 0, Process = 1, Stable = 2 };
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

inline void validate(const Key& key) {
    switch (key.kind) {
        case Kind::Text:
            if (key.name.size() > kTextBytes || !validUtf8(key.name))
                throw std::invalid_argument("invalid UTF-8 or oversized String key");
            return;
        case Kind::Process:
        case Kind::Stable:
            if (key.name.empty() || key.name.size() > kSymbolBytes)
                throw std::invalid_argument("Symbol name exceeds 1..256 byte budget");
            // TODO: replace ASCII-only gate with pinned Unicode NFC tables.
            for (unsigned char c : key.name) {
                if (c < 0x20u || c > 0x7eu)
                    throw std::invalid_argument("non-ASCII Symbol needs NFC verification");
            }
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

inline std::vector<Key> decodeDistinct(const std::vector<std::vector<std::uint8_t>>& frames) {
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
        auto key = decode(frame);
        if (!seen.insert(key).second)
            throw std::invalid_argument("duplicate logical property key");
        keys.push_back(std::move(key));
    }
    return keys;
}

} // namespace oreslang::runtime::symbol_key_v1
