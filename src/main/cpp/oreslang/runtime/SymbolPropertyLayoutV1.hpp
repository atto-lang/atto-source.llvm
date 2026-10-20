#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::symbol_property_v1 {

inline constexpr std::size_t MAX_STATIC_FIELDS = 4096;
inline constexpr std::size_t MAX_STRING_KEY_BYTES = 16384;
inline constexpr std::size_t MAX_ENCODED_KEY_BYTES = 1048576;
inline constexpr std::size_t KEY_FRAME_BYTES = 6;

// HOST-ONLY canonical key/slot proof. Guest Symbol parsing, Unicode NFC,
// LLVM lowering, untrusted actor admission and wire coding are NOT enabled.
// Native Symbol names fail closed on non-ASCII until pinned NFC parity exists.
enum class Domain : std::uint8_t { String = 0, ProcessSymbol = 1, StableSymbol = 2 };

inline bool valid_utf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        unsigned int codepoint = 0;
        unsigned int minimum = 0;
        int remaining = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            codepoint = lead & 0x1f; minimum = 0x80; remaining = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            codepoint = lead & 0x0f; minimum = 0x800; remaining = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            codepoint = lead & 0x07; minimum = 0x10000; remaining = 3;
        } else return false;
        if (i + static_cast<std::size_t>(remaining) > text.size()) return false;
        for (int k = 0; k < remaining; ++k) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}

inline bool canonical_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

class Key {
public:
    Domain domain() const noexcept { return domain_; }
    const std::string& name() const noexcept { return name_; }

    static Key string(std::string name) {
        if (name.size() > MAX_STRING_KEY_BYTES)
            throw std::invalid_argument("String property key exceeds 16384 UTF-8 bytes");
        if (!valid_utf8(name)) throw std::invalid_argument("malformed UTF-8 String key");
        return Key(Domain::String, std::move(name));
    }

    static Key process_symbol(std::string name) {
        if (name.empty() || name.size() > 256) {
            throw std::invalid_argument("Symbol length must be 1..256 UTF-8 bytes");
        }
        for (const unsigned char byte : name) {
            if (byte < 0x20 || byte >= 0x7f) {
                throw std::invalid_argument("native Symbol requires printable canonical ASCII");
            }
        }
        return Key(Domain::ProcessSymbol, std::move(name));
    }

    static Key stable_symbol(std::string uuid) {
        if (!canonical_uuid(uuid)) throw std::invalid_argument("noncanonical stable UUID");
        return Key(Domain::StableSymbol, std::move(uuid));
    }

    friend bool operator==(const Key& a, const Key& b) noexcept {
        return a.domain_ == b.domain_ && a.name_ == b.name_;
    }
private:
    Key(Domain domain, std::string name) : domain_(domain), name_(std::move(name)) {}
    Domain domain_;
    std::string name_;
};

struct KeyLess {
    bool operator()(const Key& a, const Key& b) const noexcept {
        if (a.domain() != b.domain()) {
            return static_cast<std::uint8_t>(a.domain()) < static_cast<std::uint8_t>(b.domain());
        }
        return std::lexicographical_compare(a.name().begin(), a.name().end(),
                b.name().begin(), b.name().end(), [](char x, char y) {
                    return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
                });
    }
};

class Layout {
public:
    static Layout declared(std::vector<Key> keys) {
        return Layout(std::move(keys), false);
    }
    static Layout canonical(std::vector<Key> keys) {
        return Layout(std::move(keys), true);
    }
    std::size_t size() const noexcept { return keys_.size(); }
    const std::vector<Key>& keys() const noexcept { return keys_; }
    std::size_t require_slot(const Key& key) const {
        auto it = slot_by_key_.find(key);
        if (it == slot_by_key_.end()) throw std::out_of_range("unknown property key");
        return it->second;
    }
private:
    explicit Layout(std::vector<Key> keys, bool sort) : keys_(std::move(keys)) {
        if (keys_.size() > MAX_STATIC_FIELDS)
            throw std::invalid_argument("static property layout exceeds 4096 fields");
        std::size_t bytes = 0;
        for (const auto& key : keys_) {
            const auto entry = KEY_FRAME_BYTES + key.name().size();
            if (entry > MAX_ENCODED_KEY_BYTES - bytes)
                throw std::invalid_argument("static property layout exceeds 1 MiB tagged-key budget");
            bytes += entry;
        }
        if (sort) std::sort(keys_.begin(), keys_.end(), KeyLess{});
        for (std::size_t slot = 0; slot < keys_.size(); ++slot) {
            if (!slot_by_key_.emplace(keys_[slot], slot).second)
                throw std::invalid_argument("duplicate logical property key");
        }
    }
    std::vector<Key> keys_;
    std::map<Key, std::size_t, KeyLess> slot_by_key_;
};

} // namespace oreslang::symbol_property_v1
