#pragma once
// M1 compile-time descriptor emitter for validated native AttoLang ASTs.
// Metadata-only: no MCP listener, guest reflection, JIT or generated runtime
// invocation. The compiler alone may call this after semantic admission.
#include "oreslang/runtime/McpDevControlM0.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::runtime::mcp_m1 {

inline std::string hex_id(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (unsigned i = 0; i < 16; ++i) {
        out[15u-i] = kDigits[value & 0xfu];
        value >>= 4u;
    }
    return out;
}
inline bool name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    if (!(name.front() == '_' ||
          (name.front() >= 'A' && name.front() <= 'Z') ||
          (name.front() >= 'a' && name.front() <= 'z'))) return false;
    for (char ch : name) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}
template <typename ValidatedProgram>
inline std::string emit_json(const ValidatedProgram& program) {
    // The initial M1 slice deliberately admits only public plain functions.
    // Actor state, methods, generators and private fields are never reflected.
    if (!program.actors.empty() || !program.classes.empty() ||
        !program.cooperative_loops.empty() || !program.generators.empty())
        throw std::runtime_error("mcp descriptor mode requires scalar-only public functions");
    if ((!program.module.empty() && !name_ok(program.module)) ||
        (!program.namespace_name.empty() && !name_ok(program.namespace_name)))
        throw std::runtime_error("unsupported module or namespace descriptor identity");

    struct Entry { std::string name; std::size_t arity; std::string hex; };
    std::vector<Entry> entries;
    for (const auto& f : program.functions) {
        if (!f.exported) continue;
        if (f.native_internal || !f.method_owner.empty() || !name_ok(f.name) ||
            f.parameters.size() > 32 || !f.generic_parameters.empty())
            throw std::runtime_error("unsupported public function in mcp descriptor mode");
        if (entries.size() >= 32)
            throw std::runtime_error("mcp public descriptor count exceeds 32");
        const std::string key = std::string("atto.mcp.aot-descriptors.m1.v1/") +
            program.namespace_name + "/" + program.module + "/" + f.name + "/" +
            std::to_string(f.parameters.size());
        entries.push_back(Entry{f.name,f.parameters.size(),
            hex_id(mcp_m0::stable_id(key))});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs){
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        return lhs.arity < rhs.arity;
    });
    for (std::size_t i=1; i<entries.size(); ++i) {
        if (entries[i-1].name == entries[i].name &&
            entries[i-1].arity == entries[i].arity)
            throw std::runtime_error("duplicate exported descriptor identity");
    }
    std::string out =
        "{\"schema\":\"atto.mcp.aot-descriptors.m1.v1\","
        "\"backend\":\"llvm\",\"visibility\":\"public_only\","
        "\"retention\":\"opt_in\",\"executed_guest\":false,"
        "\"abi\":\"atto.mcp.native.descriptor.v1\","
        "\"generation_binding\":\"host_runtime_required\","
        "\"descriptors\":[";
    for (std::size_t i=0; i<entries.size(); ++i) {
        if (i) out += ',';
        const auto& entry = entries[i];
        // Identifier constraints make direct JSON literals safe: no quoted,
        // escaped, control, Unicode or path-bearing source strings survive.
        out += "{\"id\":\"" + entry.hex + "\",\"kind\":\"function\","
               "\"name\":\"" + entry.name + "\",\"arity\":" +
               std::to_string(entry.arity) + ",\"public\":true}";
    }
    out += "]}\n";
    return out;
}
} // namespace oreslang::runtime::mcp_m1
