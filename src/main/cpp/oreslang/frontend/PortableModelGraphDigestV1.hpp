#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace oreslang::model::portable_v1 {

struct BufferPort {
    std::string dtype;
    std::vector<std::uint64_t> shape;
    std::optional<std::string> layout;
    std::optional<std::string> unit;
};
struct NominalPort { std::string type_abi; std::uint64_t bytes = 0; };
using Port = std::variant<BufferPort, NominalPort>;
struct Constant { std::string kind; std::string value; };
struct Profile { bool freestanding = false; std::string allocation; bool deterministic = false; };
struct Signature {
    std::vector<Port> inputs;
    Port output;
    std::string effect;
    bool vjp = false;
    std::uint64_t scratch_bytes = 0;
    std::string abi;
    std::string impl_digest;
    std::string provenance;
    std::vector<std::string> const_kinds;
    Profile profile;
};
struct Node {
    std::string id;
    std::string op;
    std::vector<std::string> inputs;
    std::vector<Constant> const_args;
    Signature signature;
};
struct Graph {
    std::uint32_t version = 1;
    std::string model_type_id;
    std::vector<Node> nodes;
    std::vector<std::string> outputs;
};

[[noreturn]] inline void fail(const char* code) { throw std::runtime_error(code); }

inline bool valid_utf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned c = p[i++];
        if (c <= 0x7f) continue;
        unsigned need = 0, lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) need = 1;
        else if (c == 0xe0) { need = 2; lo = 0xa0; }
        else if (c >= 0xe1 && c <= 0xec) need = 2;
        else if (c == 0xed) { need = 2; hi = 0x9f; }
        else if (c >= 0xee && c <= 0xef) need = 2;
        else if (c == 0xf0) { need = 3; lo = 0x90; }
        else if (c >= 0xf1 && c <= 0xf3) need = 3;
        else if (c == 0xf4) { need = 3; hi = 0x8f; }
        else return false;
        if (i + need > s.size() || p[i] < lo || p[i] > hi) return false;
        ++i;
        for (unsigned j = 1; j < need; ++j, ++i)
            if (p[i] < 0x80 || p[i] > 0xbf) return false;
    }
    return true;
}

inline int ordinal(const std::string& v, std::initializer_list<const char*> values,
                   const char* code) {
    int n = 0;
    for (const char* x : values) { if (v == x) return n; ++n; }
    fail(code);
}

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

struct Bytes {
    std::vector<std::uint8_t> data;
    void u8(std::uint8_t v) { data.push_back(v); }
    void u32(std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(v >> shift));
    }
    void u64(std::uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(v >> shift));
    }
    void raw(const std::string& s) { data.insert(data.end(), s.begin(), s.end()); }
    void string(const std::string& s) {
        if (!valid_utf8(s) || s.size() > 0xffff'ffffULL) fail("MODEL_DIGEST_STRING");
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s);
    }
    void digest(const std::string& s) {
        if (s.size() != 64) fail("MODEL_INVALID_IMPLEMENTATION_DIGEST");
        for (std::size_t i = 0; i < 64; i += 2) {
            int a = hex_nibble(s[i]), b = hex_nibble(s[i+1]);
            if (a < 0 || b < 0) fail("MODEL_INVALID_IMPLEMENTATION_DIGEST");
            u8(static_cast<std::uint8_t>((a << 4) | b));
        }
    }
};

inline bool canonical_i64(const std::string& s, std::int64_t& value) {
    if (s.empty() || s[0] == '+') return false;
    std::size_t first = s[0] == '-' ? 1 : 0;
    if (first == s.size()) return false;
    if (s[first] == '0' && first + 1 != s.size()) return false;
    for (std::size_t i = first; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
