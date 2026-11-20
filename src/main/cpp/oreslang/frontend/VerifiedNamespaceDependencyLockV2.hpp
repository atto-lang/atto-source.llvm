#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// PREPARATORY HOST ONLY: lock bytes/digest must originate from an independent,
// approved trust root. A caller computing the expected digest from an untrusted
// file has NOT authenticated the lock. This never grants LLVM IR/codegen rights.
namespace oreslang::frontend {
using NamespaceLockHashV2 = std::function<std::string(std::string_view)>;
using NamespaceLockPinsV2 = std::map<std::string, std::map<std::string, std::string>>;
class VerifiedNamespaceDependencyLockV2 {
public:
    const NamespaceLockPinsV2& alias_pins() const { return pins_; }
    const std::string& lock_sha256() const { return hash_; }
private:
    VerifiedNamespaceDependencyLockV2() = default;
    NamespaceLockPinsV2 pins_;
    std::string hash_;
    friend VerifiedNamespaceDependencyLockV2 verify_namespace_dependency_lock_v2(
        std::string_view, std::string_view, const std::set<std::string>&,
        const NamespaceLockHashV2&);
};
inline bool namespace_lock_hex_v2(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool namespace_lock_name_v2(std::string_view s) {
    if (s.empty() || s.size() > 128 || !((s[0] >= 'a' && s[0] <= 'z') ||
            (s[0] >= 'A' && s[0] <= 'Z'))) return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
inline bool namespace_lock_owner_v2(std::string_view s) {
    if (s.substr(0, 3) != "v2:" || s.size() < 3 + 64 + 1 + 64 + 1 + 1) return false;
    if (!namespace_lock_hex_v2(s.substr(3, 64)) || s[67] != ':' ||
            !namespace_lock_hex_v2(s.substr(68, 64)) || s[132] != ':') return false;
    return namespace_lock_name_v2(s.substr(133));
}
inline std::string namespace_lock_trim_v2(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return std::string(value.substr(first, last + 1 - first));
}
inline VerifiedNamespaceDependencyLockV2 verify_namespace_dependency_lock_v2(
        std::string_view bytes, std::string_view expected_sha256,
        const std::set<std::string>& authorized_physical_owners,
        const NamespaceLockHashV2& sha256) {
    if (!sha256 || !namespace_lock_hex_v2(expected_sha256) ||
            bytes.empty() || bytes.size() > 65536 || sha256(bytes) != expected_sha256)
        throw std::invalid_argument("v2 dependency lock requires externally pinned SHA-256");
    for (const auto& owner : authorized_physical_owners)
        if (!namespace_lock_owner_v2(owner))
            throw std::invalid_argument("v2 lock supplied invalid verified package owner");
    VerifiedNamespaceDependencyLockV2 result;
    result.hash_ = std::string(expected_sha256);
    bool version = false, in_entry = false;
    std::map<std::string, std::string> entry;
    const auto commit = [&] {
        if (!in_entry) return;
        if (entry.size() != 3 || !entry.count("importer") || !entry.count("alias") ||
                !entry.count("target"))
            throw std::invalid_argument("v2 dependency lock entry is incomplete");
        const auto& importer = entry.at("importer");
        const auto& target = entry.at("target");
        const auto& alias = entry.at("alias");
        if (!namespace_lock_owner_v2(importer) || !namespace_lock_owner_v2(target) ||
                !namespace_lock_name_v2(alias) || importer == target ||
                authorized_physical_owners.count(importer) == 0 ||
                authorized_physical_owners.count(target) == 0)
            throw std::invalid_argument("v2 dependency lock owner/alias lacks verified package identity");
        if (!result.pins_[importer].emplace(alias, target).second)
            throw std::invalid_argument("v2 dependency lock duplicate importer alias");
        if (result.pins_.size() > 256)
            throw std::invalid_argument("v2 dependency lock importer budget exceeded");
        entry.clear();
    };
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const auto next = bytes.find('\n', pos);
        const auto n = next == bytes.npos ? bytes.size() - pos : next - pos;
        auto line = namespace_lock_trim_v2(bytes.substr(pos, n));
        if (!line.empty() && line[0] != '#') {
            for (unsigned char c : line)
                if (c < 32 || c > 126 || c == '\\' || c == '\'')
                    throw std::invalid_argument("v2 dependency lock unsupported TOML character");
            if (line == "[[dependency]]") {
                if (!version) throw std::invalid_argument("v2 dependency lock missing version");
                commit();
                in_entry = true;
            } else {
                const auto sep = line.find(" = ");
                if (sep == line.npos || line.find('=', sep + 3) != line.npos)
                    throw std::invalid_argument("v2 dependency lock invalid assignment");
                const auto key = line.substr(0, sep);
                const auto value = line.substr(sep + 3);
                if (value.size() < 2 || value.front() != '"' || value.back() != '"')
                    throw std::invalid_argument("v2 dependency lock requires quoted scalar");
                const auto str = value.substr(1, value.size() - 2);
                if (!in_entry) {
                    if (version || key != "schema_version" || str != "1")
                        throw std::invalid_argument("v2 dependency lock unsupported schema");
                    version = true;
                } else {
                    if (key != "importer" && key != "alias" && key != "target")
                        throw std::invalid_argument("v2 dependency lock unknown entry field");
                    if (!entry.emplace(key, str).second)
                        throw std::invalid_argument("v2 dependency lock duplicate entry field");
                }
            }
        }
        if (next == bytes.npos) break;
        pos = next + 1;
    }
    commit();
    if (!version || result.pins_.empty())
        throw std::invalid_argument("v2 dependency lock has no verified entries");
    return result;
}
} // namespace oreslang::frontend
