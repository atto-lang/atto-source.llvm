#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"
#include <charconv>
#include <cstdint>
#include <initializer_list>
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

inline bool canonical_i64_syntax(const std::string& s) {
    if (s.empty() || s[0] == '+') return false;
    std::size_t first = s[0] == '-' ? 1 : 0;
    if (first == s.size()) return false;
    if (s[first] == '0' && first + 1 != s.size()) return false;
    for (std::size_t i = first; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

inline void encode_constant(Bytes& out, const Constant& c) {
    out.u8(static_cast<std::uint8_t>(ordinal(c.kind,
        {"bool","i64","f64bits","string","symbol"}, "MODEL_INVALID_CONST")));
    if (c.kind == "bool") {
        if (c.value != "true" && c.value != "false") fail("MODEL_INVALID_CONST");
        out.u8(c.value == "true" ? 1 : 0);
    } else if (c.kind == "i64") {
        if (!canonical_i64_syntax(c.value)) fail("MODEL_INVALID_CONST");
        std::int64_t v = 0;
        auto parsed = std::from_chars(c.value.data(), c.value.data() + c.value.size(), v);
        if (parsed.ec == std::errc::result_out_of_range) fail("MODEL_CONST_OVERFLOW");
        if (parsed.ec != std::errc{} || parsed.ptr != c.value.data() + c.value.size())
            fail("MODEL_INVALID_CONST");
        out.u64(static_cast<std::uint64_t>(v));
    } else if (c.kind == "f64bits") {
        if (c.value.size() != 18 || c.value[0] != '0' || c.value[1] != 'x')
            fail("MODEL_INVALID_CONST");
        for (std::size_t i = 2; i < c.value.size(); ++i)
            if (hex_nibble(c.value[i]) < 0) fail("MODEL_INVALID_CONST");
        std::uint64_t bits = 0;
        auto result = std::from_chars(c.value.data() + 2, c.value.data() + 18, bits, 16);
        if (result.ec != std::errc{} || result.ptr != c.value.data() + 18)
            fail("MODEL_INVALID_CONST");
        if (((bits >> 52) & 0x7ffULL) == 0x7ffULL) fail("MODEL_NONFINITE_CONST");
        out.u64(bits);
    } else if (c.kind == "string" || c.kind == "symbol") {
        out.string(c.value);
    } else fail("MODEL_INVALID_CONST");
}

inline void encode_port(Bytes& out, const Port& port) {
    if (std::holds_alternative<NominalPort>(port)) {
        const auto& p = std::get<NominalPort>(port);
        out.u8(1); out.string(p.type_abi); out.u64(p.bytes); return;
    }
    const auto& p = std::get<BufferPort>(port);
    if (p.shape.size() > 8) fail("MODEL_INVALID_PORT");
    out.u8(0);
    out.u8(static_cast<std::uint8_t>(ordinal(p.dtype,
        {"bool","i32","i64","f32","f64"}, "MODEL_INVALID_PORT")));
    out.u8(static_cast<std::uint8_t>(p.shape.size()));
    for (auto d : p.shape) out.u64(d);
    const std::string layout = p.layout ? *p.layout : (p.shape.empty() ? "scalar" : "contiguous");
    if ((p.shape.empty() && layout != "scalar") || (!p.shape.empty() && layout != "contiguous"))
        fail("MODEL_INVALID_PORT");
    out.u8(static_cast<std::uint8_t>(ordinal(layout, {"scalar","contiguous"}, "MODEL_INVALID_PORT")));
    if (!p.unit) out.u8(0);
    else { out.u8(1); out.string(*p.unit); }
}

inline std::vector<std::uint8_t> canonical_bytes(const Graph& graph) {
    if (graph.version != 1 || graph.nodes.empty() || graph.outputs.empty())
        fail("MODEL_INVALID_RESOLVED_GRAPH");
    Bytes out;
    const std::string prefix("ORESLANG-MODEL-GRAPH-V1\0", 24);
    out.data.insert(out.data.end(), prefix.begin(), prefix.end());
    out.u32(1);
    out.string(graph.model_type_id);

    std::unordered_map<std::string,std::uint32_t> ordinals;
    for (std::uint32_t i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].id.empty() || !ordinals.emplace(graph.nodes[i].id, i).second)
            fail("MODEL_DUPLICATE_NODE");
    }

    out.u32(static_cast<std::uint32_t>(graph.nodes.size()));
    for (std::uint32_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        const auto& sig = node.signature;
        if (node.const_args.size() != sig.const_kinds.size()) fail("MODEL_CONST_ARITY_MISMATCH");
        out.string(node.op);
        out.string(sig.abi);
        out.digest(sig.impl_digest);
        out.u8(static_cast<std::uint8_t>(ordinal(sig.provenance,
            {"verified-ores","trusted-native"}, "MODEL_INVALID_OP_SIGNATURE")));
        out.u8(static_cast<std::uint8_t>(ordinal(sig.effect,
            {"pure","io","network","random","clock","actor","ffi","suspend"},
            "MODEL_INVALID_OP_SIGNATURE")));
        out.u8(sig.vjp ? 1 : 0);
        out.u32(static_cast<std::uint32_t>(sig.const_kinds.size()));
        for (const auto& kind : sig.const_kinds)
            out.u8(static_cast<std::uint8_t>(ordinal(kind,
                {"bool","i64","f64bits","string","symbol"}, "MODEL_INVALID_OP_SIGNATURE")));
        out.u32(static_cast<std::uint32_t>(node.const_args.size()));
        for (std::size_t i = 0; i < node.const_args.size(); ++i) {
            if (node.const_args[i].kind != sig.const_kinds[i]) fail("MODEL_CONST_TYPE_MISMATCH");
            encode_constant(out, node.const_args[i]);
        }
        out.u32(static_cast<std::uint32_t>(node.inputs.size()));
        for (const auto& parent : node.inputs) {
            auto it = ordinals.find(parent);
            if (it == ordinals.end() || it->second >= index) fail("MODEL_FORWARD_OR_CYCLIC_EDGE");
            out.u32(it->second);
        }
        out.u32(static_cast<std::uint32_t>(sig.inputs.size()));
        for (const auto& input : sig.inputs) encode_port(out, input);
        encode_port(out, sig.output);
        out.u64(sig.scratch_bytes);
        out.u8(sig.profile.freestanding ? 1 : 0);
        out.u8(static_cast<std::uint8_t>(ordinal(sig.profile.allocation,
            {"none","scratch","dynamic"}, "MODEL_INVALID_OP_PROFILE")));
        out.u8(sig.profile.deterministic ? 1 : 0);
    }

    std::unordered_set<std::string> seen;
    out.u32(static_cast<std::uint32_t>(graph.outputs.size()));
    for (const auto& output : graph.outputs) {
        auto it = ordinals.find(output);
        if (it == ordinals.end()) fail("MODEL_UNKNOWN_OUTPUT");
        if (!seen.insert(output).second) fail("MODEL_DUPLICATE_OUTPUT");
        out.u32(it->second);
    }
    return out.data;
}

inline std::string digest(const Graph& graph) {
    const auto bytes = canonical_bytes(graph);
    const auto hash = llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(bytes.data(), bytes.size()));
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto byte : hash) {
        out.push_back(hex[(byte >> 4) & 0x0f]);
        out.push_back(hex[byte & 0x0f]);
    }
    return out;
}

} // namespace oreslang::model::portable_v1
