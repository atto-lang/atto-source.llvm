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
#include <unordered_set>

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

    // Opaque, deterministic identity only. Never use a descriptor ID as
    // authentication, runtime generation authority, or an ABI capability.
    const auto module_id = hex_id(mcp_m0::stable_id(
        std::string("atto.mcp.module.v1/") + program.namespace_name +
        "/" + program.module));
    struct Entry { std::string name; std::size_t arity; std::string hex; };
    std::vector<Entry> entries;
    for (const auto& f : program.functions) {
        if (!f.exported) continue;
        // M1 descriptors describe only plain, synchronous, value-parameter
        // scalar functions. A valid compiled Task/actor/effect is NOT an
        // authority to expose its runtime state through MCP metadata.
        if (f.native_internal || !f.method_owner.empty() || !name_ok(f.name) ||
            f.parameters.size() > 32 || !f.generic_parameters.empty() ||
            f.scalar_while || f.async_source_i64 || !f.source_tasks.empty() ||
            !f.future_pipelines.empty() || !f.actor_spawns.empty() ||
            !f.actor_sends.empty() || !f.actor_stops.empty() ||
            !f.actor_actions.empty() || !f.actor_field_writes.empty() ||
            !f.ready_await_actor.empty() || !f.done_await_actor.empty() ||
            !f.ready_future_name.empty() || f.has_declared_effects ||
            f.caught_throw_value || !f.caught_throw_defers.empty() ||
            f.returns_void || !f.return_class.empty() ||
            !f.return_sum.empty() || !f.pending_return_type_alias.empty())
            throw std::runtime_error("unsupported public function in mcp descriptor mode");
        for (const auto& parameter : f.parameters) {
            if (!parameter.class_name.empty() || !parameter.generic_type.empty() ||
                !parameter.sum_type.empty() || !parameter.pending_type_alias.empty() ||
                parameter.mode != decltype(parameter.mode)::Value)
                throw std::runtime_error("non-scalar or borrowed MCP public parameter");
        }
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
    std::unordered_set<std::string> ids;
    for (std::size_t i=0; i<entries.size(); ++i) {
        if (i && entries[i-1].name == entries[i].name &&
            entries[i-1].arity == entries[i].arity)
            throw std::runtime_error("duplicate exported descriptor identity");
        if (!ids.insert(entries[i].hex).second)
            throw std::runtime_error("colliding exported descriptor ID");
    }
    std::string out =
        "{\"schema\":\"atto.mcp.aot-descriptors.m1.v1\","
        "\"backend\":\"llvm\",\"visibility\":\"public_only\","
        "\"retention\":\"opt_in\",\"executed_guest\":false,"
        "\"abi\":\"atto.mcp.native.descriptor.v1\","
        "\"generation_binding\":\"host_runtime_required\","
        "\"module_id\":\"" + module_id + "\","
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
