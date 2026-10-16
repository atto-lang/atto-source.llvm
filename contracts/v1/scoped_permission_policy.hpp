#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>  // Numeric address parsing only; NO DNS lookup.
#endif

// Native host policy substrate only. This does not intercept OS syscalls,
// guest code or an actor's I/O; callers must enforce it at every boundary.
namespace oreslang::permissions {

enum class Permission : std::uint8_t {
    Read, Write, Net, Env, Run, Sys, Ffi, Import
};

struct ScopeRule {
    Permission permission;
    std::string scope;
    bool denied = false;
};

class ScopedPolicy final {
    static constexpr std::size_t count = 8;
    static constexpr std::size_t max_rules = 512;
    std::array<bool, count> ceiling_{};
    std::array<std::vector<std::string>, count> allowed_{};
    std::array<std::vector<std::string>, count> denied_{};

    static std::size_t index(Permission p) {
        const auto i = static_cast<unsigned>(p);
        if (i >= count) throw std::invalid_argument("unknown permission kind");
        return i;
    }

    static bool control(std::string_view s) {
        for (unsigned char c : s) if (c < 32 || c == 127) return true;
        return false;
    }

    static bool path_valid(std::string_view s) {
        if (s.empty() || s.size() > 4096 || s[0] != '/' || control(s)) return false;
        if (s.size() > 1 && s.back() == '/') return false;
        std::size_t begin = 1;
        while (begin < s.size()) {
            const auto slash = s.find('/', begin);
            auto part = s.substr(begin, slash == s.npos ? s.npos : slash - begin);
            if (part.empty() || part == "." || part == "..") return false;
            if (slash == s.npos) break;
            begin = slash + 1;
        }
        return true;
    }

    static bool token_valid(std::string_view s, bool scope) {
        if (s.empty() || s.size() > 1024 || control(s)) return false;
        auto star = s.find('*');
        if (star == s.npos) return true;
        return scope && (s == "*" || (star == s.size() - 1 && s.find('*', star + 1) == s.npos));
    }

    struct Endpoint {
        std::string host;
        unsigned port = 0;
        bool has_port = false;
        bool subdomain = false;
    };

    static bool network_parse(std::string_view raw, bool scope, Endpoint& out) {
        if (raw.empty() || raw.size() > 320 || control(raw) ||
            raw.find_first_of(" \t\n\r/@\\") != raw.npos)
            return false;
        std::string s(raw);
        for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (scope && s == "*") { out.host = "*"; return true; }
        if (s.front() == '[') {
#if defined(__unix__) || defined(__APPLE__)
            // Require brackets around all IPv6 endpoints. Parse binary identity
            // without DNS or libc hostname resolution; deny mapped IPv4 and
            // zone-scoped addresses until separately authorized.
            const auto closing = s.find(']');
            if (closing == s.npos || closing <= 1) return false;
            const std::string literal = s.substr(1, closing - 1);
            if (literal.find('.') != std::string::npos) return false; // No embedded IPv4.
            std::array<unsigned char, 16> addr{};
            if (inet_pton(AF_INET6, literal.c_str(), addr.data()) != 1)
                return false;
            bool mapped = true;
            for (std::size_t i = 0; i < 10; ++i)
                mapped = mapped && addr[i] == 0;
            if (mapped && addr[10] == 0xff && addr[11] == 0xff)
                return false;
            if (closing + 1 < s.size()) {
                if (s[closing + 1] != ':' || closing + 2 == s.size())
                    return false;
                unsigned port = 0;
                for (std::size_t i = closing + 2; i < s.size(); ++i) {
                    if (s[i] < '0' || s[i] > '9') return false;
                    port = port * 10 + static_cast<unsigned>(s[i] - '0');
                    if (port > 65535) return false;
                }
                if (port == 0) return false;
                out.port = port;
                out.has_port = true;
            }
            static constexpr char hex[] = "0123456789abcdef";
            out.host = "[";
            for (unsigned char byte : addr) {
                out.host.push_back(hex[byte >> 4]);
                out.host.push_back(hex[byte & 0xf]);
            }
            out.host += "]";
            return true;
#else
            return false; // Explicitly unsupported platform: deny.
#endif
        }
        auto colon = s.find(':');
        if (colon != s.npos) {
            if (s.find(':', colon + 1) != s.npos || colon + 1 == s.size()) return false;
            unsigned port = 0;
            for (auto i = colon + 1; i < s.size(); ++i) {
                const char c = s[i];
                if (c < '0' || c > '9') return false;
                port = port * 10 + static_cast<unsigned>(c - '0');
                if (port > 65535) return false;
            }
            if (port == 0) return false;
            out.port = port; out.has_port = true;
            s.resize(colon);
        }
        if (!s.empty() && s.back() == '.') s.pop_back();
        if (scope && s.rfind("*.", 0) == 0) {
            out.subdomain = true; s.erase(0, 2);
        }
        if (s.empty() || s.size() > 253 || s.front() == '.' || s.back() == '.') return false;
        bool numeric = true;
        bool numeric_address_alias = true;
        std::size_t begin = 0, segments = 0;
        do {
            auto dot = s.find('.', begin);
            auto part = std::string_view(s).substr(begin, dot == s.npos ? s.npos : dot - begin);
            if (part.empty() || part.size() > 63 || part.front() == '-' || part.back() == '-')
                return false;
            for (char c : part) {
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                    return false;
                if (c < '0' || c > '9') numeric = false;
            }
            const bool decimal_part =
                part.find_first_not_of("0123456789") == std::string_view::npos;
            const bool hexadecimal_part = part.size() > 2 &&
                part.substr(0, 2) == "0x" &&
                part.substr(2).find_first_not_of("0123456789abcdef") ==
                    std::string_view::npos;
            if (!decimal_part && !hexadecimal_part) numeric_address_alias = false;
            if (dot == s.npos) break;
            begin = dot + 1; ++segments;
        } while (true);
        if (numeric) {
            if (segments != 3) return false; // No libc numeric IPv4 aliases.
            std::size_t start = 0;
            do {
                auto end = s.find('.', start);
                auto part = std::string_view(s).substr(start, end == s.npos ? s.npos : end - start);
                if (part.size() > 3 || (part.size() > 1 && part.front() == '0')) return false;
                unsigned n = 0;
                for (char c : part) n = n * 10 + static_cast<unsigned>(c - '0');
                if (n > 255) return false;
                if (end == s.npos) break;
                start = end + 1;
            } while (true);
        }
        // libc may resolve mixed decimal/hex dotted components as IPv4
        // (127.0x0.0.1, 1.2.3.0x01), not as the hostname written here.
        // Reject every noncanonical numeric/hex-component address spelling,
        // not merely spellings beginning with 0x.
        if (!numeric && numeric_address_alias) return false;
        out.host = std::move(s);
        return true;
    }

    static bool matches(Permission kind, std::string_view resource, std::string_view scope) {
        if (scope == "*") return true;
        switch (kind) {
            case Permission::Read: case Permission::Write:
            case Permission::Ffi: case Permission::Import:
                if (resource == scope) return true;
                return scope == "/" ? resource.size() > 1 :
                    resource.size() > scope.size() && resource.substr(0, scope.size()) == scope &&
                        resource[scope.size()] == '/';
            case Permission::Net: {
                Endpoint target, rule;
                if (!network_parse(resource, false, target) || !network_parse(scope, true, rule)) return false;
                if (rule.has_port && (!target.has_port || target.port != rule.port)) return false;
                if (!rule.subdomain) return target.host == rule.host;
                return target.host.size() > rule.host.size() + 1 &&
                    target.host.compare(target.host.size() - rule.host.size(), rule.host.size(), rule.host) == 0 &&
                    target.host[target.host.size() - rule.host.size() - 1] == '.';
            }
            case Permission::Env: case Permission::Run: case Permission::Sys:
                if (scope.back() == '*') return resource.substr(0, scope.size() - 1) == scope.substr(0, scope.size() - 1);
                return resource == scope;
        }
        return false;
    }

    static bool scope_valid(Permission kind, std::string_view scope) {
        if (scope == "*") return true;
        switch (kind) {
            case Permission::Read: case Permission::Write: case Permission::Ffi:
            case Permission::Import: return path_valid(scope);
            case Permission::Net: { Endpoint endpoint; return network_parse(scope, true, endpoint); }
            case Permission::Env: case Permission::Run: case Permission::Sys:
                return token_valid(scope, true);
        }
        return false;
    }

public:
    ScopedPolicy() = default;  // Standalone default-deny.

    // Only the trusted host supplies the ceiling. Rules can never widen it.
    static ScopedPolicy from_host(std::vector<Permission> capabilities,
                                  std::vector<ScopeRule> rules) {
        if (capabilities.size() > count || rules.size() > max_rules)
            throw std::invalid_argument("oversized permission policy");
        ScopedPolicy result;
        for (auto capability : capabilities) result.ceiling_[index(capability)] = true;
        for (const auto& rule : rules) {
            const auto i = index(rule.permission);
            if (!scope_valid(rule.permission, rule.scope))
                throw std::invalid_argument("invalid permission scope");
            auto& entries = rule.denied ? result.denied_[i] : result.allowed_[i];
            entries.push_back(rule.scope);
        }
        return result;
    }

    bool allows(Permission kind, std::string_view resource) const {
        const auto i = index(kind);
        if (!ceiling_[i] || resource.empty() || control(resource)) return false;
        switch (kind) {
            case Permission::Read: case Permission::Write: case Permission::Ffi:
            case Permission::Import: if (!path_valid(resource)) return false; break;
            case Permission::Net: {
                Endpoint target;
                if (!network_parse(resource, false, target)) return false;
                // Without a target port, a matching port-specific deny must
                // not be bypassed by a broad allow-all grant.
                if (!target.has_port) {
                    for (const auto& raw : denied_[i]) {
                        Endpoint rule;
                        if (network_parse(raw, true, rule) && rule.has_port) {
                            if (!rule.subdomain && target.host == rule.host) return false;
                            if (rule.subdomain && matches(kind, resource, rule.host == "" ? "" : "*." + rule.host)) return false;
                        }
                    }
                }
                break;
            }
            case Permission::Env: case Permission::Run: case Permission::Sys:
                if (!token_valid(resource, false)) return false;
                break;
        }
        for (const auto& deny : denied_[i]) if (matches(kind, resource, deny)) return false;
        for (const auto& allow : allowed_[i]) if (matches(kind, resource, allow)) return true;
        return false;
    }

    // A *trusted* socket adapter must pass the actual DNS result selected for
    // the socket, including the chosen port, immediately before connect().
    // An allowed hostname must not override a denied resolved IP. This is
    // policy-only: it does not perform DNS, create a socket, pin an address,
    // or intercept guest calls. Noncanonical/IPv6 values fail closed for now.
    bool allows_network_connection(std::string_view requested,
                                   std::string_view selected_ipv4) const {
        if (!allows(Permission::Net, requested)) return false;
        Endpoint target, resolved;
        if (!network_parse(requested, false, target) ||
            !network_parse(selected_ipv4, false, resolved) ||
            !target.has_port || !resolved.has_port ||
            target.port != resolved.port) return false;
        // network_parse already enforces exact 4-octet decimal canonicality
        // for all-numeric hosts. Reject every nonnumeric DNS host here.
        unsigned dots = 0;
        for (char c : resolved.host) {
            if (c == '.') ++dots;
            else if (c < '0' || c > '9') return false;
        }
        if (dots != 3) return false;
        for (const auto& blocked : denied_[index(Permission::Net)]) {
            if (matches(Permission::Net, selected_ipv4, blocked)) return false;
        }
        return true;
    }
};

} // namespace oreslang::permissions
