#pragma once
#include "scoped_permission_policy.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::permissions {

// Internal serialized application policy envelope, NOT public CLI parsing.
// Accepted rules are always attenuated by an explicit trusted host ceiling.
// It does not authenticate a caller, grant ambient authority or replace flags-2-env.
inline ScopedPolicy parse_serialized_scoped_policy(
        const std::vector<std::string>& arguments,
        const ScopedPolicy& trusted_host_ceiling) {
    static constexpr std::string_view cap_prefix = "--ores-capabilities=";
    static constexpr std::string_view rule_prefix = "--ores-permission-";
    static constexpr std::string_view explicit_marker = "--ores-permission-explicit=true";
    if (arguments.size() > 514)
        throw std::invalid_argument("oversized serialized policy");
    bool saw_capabilities = false;
    bool saw_marker = false;
    std::vector<Permission> capabilities;
    std::vector<ScopeRule> rules;
    std::unordered_set<std::string> unique;
    std::size_t total_bytes = 0;
    auto cap_kind = [](std::string_view s) -> Permission {
        if (s == "FILESYSTEM_READ") return Permission::Read;
        if (s == "FILESYSTEM_WRITE") return Permission::Write;
        if (s == "NETWORK") return Permission::Net;
        if (s == "ENVIRONMENT") return Permission::Env;
        if (s == "CHILD_PROCESS") return Permission::Run;
        if (s == "PROCESS_INFO") return Permission::Sys;
        if (s == "FFI") return Permission::Ffi;
        if (s == "HOT_CODE_LOAD") return Permission::Import;
        throw std::invalid_argument("unsupported serialized capability");
    };
    auto permission_kind = [](std::string_view s) -> Permission {
        if (s == "read") return Permission::Read;
        if (s == "write") return Permission::Write;
        if (s == "net") return Permission::Net;
        if (s == "env") return Permission::Env;
        if (s == "run") return Permission::Run;
        if (s == "sys") return Permission::Sys;
        if (s == "ffi") return Permission::Ffi;
        if (s == "import") return Permission::Import;
        throw std::invalid_argument("unknown serialized permission");
    };
    for (const auto& arg : arguments) {
        if (arg.size() > 4096 || arg.size() > 1024 * 1024 - total_bytes)
            throw std::invalid_argument("oversized serialized argument");
        total_bytes += arg.size();
        if (!unique.insert(arg).second)
            throw std::invalid_argument("duplicate serialized argument");
        for (unsigned char c : arg)
            if (c < 32 || c == 127)
                throw std::invalid_argument("serialized control byte");
        if (arg == explicit_marker) {
            if (saw_marker) throw std::invalid_argument("duplicate permission marker");
            saw_marker = true;
            continue;
        }
        if (arg.rfind(cap_prefix, 0) == 0) {
            if (saw_capabilities) throw std::invalid_argument("duplicate capability header");
            saw_capabilities = true;
            std::string_view csv(arg.data() + cap_prefix.size(), arg.size() - cap_prefix.size());
            if (csv.empty()) continue;
            std::unordered_set<std::string> seen_caps;
            while (!csv.empty()) {
                const auto comma = csv.find(',');
                const auto one = csv.substr(0, comma);
                if (one.empty() || !seen_caps.insert(std::string(one)).second)
                    throw std::invalid_argument("empty or duplicate capability");
                capabilities.push_back(cap_kind(one));
                if (comma == csv.npos) break;
                csv.remove_prefix(comma + 1);
                if (csv.empty()) throw std::invalid_argument("trailing capability comma");
            }
            continue;
        }
        if (arg.rfind(rule_prefix, 0) != 0)
            throw std::invalid_argument("unknown serialized policy argument");
        std::string_view remainder(arg.data() + rule_prefix.size(), arg.size() - rule_prefix.size());
        const auto equal = remainder.find('=');
        if (equal == remainder.npos || equal == 0)
            throw std::invalid_argument("missing serialized rule value");
        const auto key = remainder.substr(0, equal);
        const auto value = remainder.substr(equal + 1);
        bool deny = false;
        std::string_view name;
        if (key.size() > 6 && key.substr(key.size() - 6) == "-allow")
            name = key.substr(0, key.size() - 6);
        else if (key.size() > 5 && key.substr(key.size() - 5) == "-deny") {
            name = key.substr(0, key.size() - 5);
            deny = true;
        } else throw std::invalid_argument("invalid serialized rule type");
        if (value.empty()) throw std::invalid_argument("empty serialized rule scope");
        rules.push_back({permission_kind(name), std::string(value), deny});
        if (rules.size() > 512) throw std::invalid_argument("too many serialized rules");
    }
    if (!saw_capabilities || !saw_marker)
        throw std::invalid_argument("capability header and explicit marker are required");
    const auto has_cap = [&](Permission p) {
        for (auto cap : capabilities) if (cap == p) return true;
        return false;
    };
    for (const auto& rule : rules)
        if (!rule.denied && !has_cap(rule.permission))
            throw std::invalid_argument("serialized rule exceeds coarse capabilities");
    return ScopedPolicy::from_host(std::move(capabilities), std::move(rules))
        .bounded_by(trusted_host_ceiling);
}
} // namespace oreslang::permissions
