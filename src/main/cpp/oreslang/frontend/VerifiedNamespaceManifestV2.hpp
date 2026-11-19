#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Deliberately conservative v2 TOML inventory subset. A strict whole-file
// parser is preferable to extracting package entries with ambiguous regexes.
// No filesystem authority or native codegen is granted by this API: callers
// must provide immutable bytes from independently checked physical files and
// a real cryptographic SHA-256 implementation.
namespace oreslang::frontend {

using NamespaceSha256 = std::function<std::string(std::string_view)>;

// Only the verifier can construct an admitted inventory. Returning mutable
// maps here would let a caller silently add provider paths after checking the
// manifest and source digest. This is a snapshot, not a builder.
class VerifiedNamespaceManifestV2 {
public:
    const std::map<std::string, std::vector<std::string>>& package_files() const {
        return package_files_;
    }
    const std::string& manifest_sha256() const { return manifest_sha256_; }
    const std::string& sources_sha256() const { return sources_sha256_; }
private:
    VerifiedNamespaceManifestV2() = default;
    std::map<std::string, std::vector<std::string>> package_files_;
    std::string manifest_sha256_;
    std::string sources_sha256_;
    friend VerifiedNamespaceManifestV2 verify_namespace_manifest_v2(
        std::string_view, std::string_view, const std::map<std::string, std::string>&,
        std::string_view, const NamespaceSha256&);
};

inline bool namespace_v2_digest(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool namespace_v2_name(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    if (!((s.front() >= 'A' && s.front() <= 'Z')
            || (s.front() >= 'a' && s.front() <= 'z'))) return false;
    for (unsigned char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
inline bool namespace_v2_relative_source(std::string_view s) {
    if (s.empty() || s.size() > 4096 || s[0] == '/' || s.find('\\') != s.npos
            || s.find(':') != s.npos || s.size() < 6
            || s.substr(s.size() - 5) != ".ores") return false;
    std::size_t begin = 0;
    while (begin < s.size()) {
        const auto end = s.find('/', begin);
        const auto part = s.substr(begin, end == s.npos ? s.size() - begin : end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        if (end == s.npos) break;
        begin = end + 1;
    }
    return true;
}
inline std::string namespace_v2_trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}
inline std::string namespace_v2_strip_comment(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (!quoted && line[i] == '#') return line.substr(0, i);
        if (static_cast<unsigned char>(line[i]) < 32 && line[i] != '\t' && line[i] != '\r')
            throw std::invalid_argument("v2 manifest contains control character");
        if (line[i] == '\\' || line[i] == '\'')
            throw std::invalid_argument("v2 inventory subset excludes escaping and literal strings");
    }
    if (quoted) throw std::invalid_argument("v2 manifest has unterminated string");
    return line;
}
inline std::vector<std::string> namespace_v2_parse_files(std::string_view s) {
    if (s.size() < 2 || s.front() != '[' || s.back() != ']')
        throw std::invalid_argument("v2 package files requires a closed array");
    std::size_t i = 1;
    std::vector<std::string> paths;
    while (true) {
        while (i + 1 < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i == s.size() - 1) break;
        if (s[i++] != '"') throw std::invalid_argument("v2 package files need quoted paths");
        const auto begin = i;
        while (i + 1 < s.size() && s[i] != '"') ++i;
        if (i == s.size() - 1) throw std::invalid_argument("v2 package path string is unterminated");
        const auto item = s.substr(begin, i - begin);
        if (!namespace_v2_relative_source(item))
            throw std::invalid_argument("v2 package file is noncanonical or unsafe");
        paths.emplace_back(item);
        if (paths.size() > 4096) throw std::invalid_argument("v2 source inventory exceeds 4096 files");
        ++i;
        while (i + 1 < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i == s.size() - 1) break;
        if (s[i++] != ',') throw std::invalid_argument("v2 package paths must be comma-separated");
    }
    if (paths.empty()) throw std::invalid_argument("v2 package needs source files");
    return paths;
}

inline VerifiedNamespaceManifestV2 verify_namespace_manifest_v2(
        std::string_view manifest_bytes,
        std::string_view expected_manifest_sha256,
        const std::map<std::string, std::string>& admitted_sources,
        std::string_view expected_sources_sha256,
        const NamespaceSha256& sha256) {
    if (!sha256 || !namespace_v2_digest(expected_manifest_sha256)
            || !namespace_v2_digest(expected_sources_sha256))
        throw std::invalid_argument("v2 manifest requires trusted SHA-256 pins and hasher");
    if (manifest_bytes.empty() || manifest_bytes.size() > 1024 * 1024)
        throw std::invalid_argument("v2 namespace manifest exceeds byte budget");
    if (sha256(manifest_bytes) != expected_manifest_sha256)
        throw std::invalid_argument("v2 namespace manifest SHA-256 mismatch");

    std::string section, pending;
    bool saw_schema = false, saw_project_root = false;
    std::set<std::string> seen_packages, seen_files;
    VerifiedNamespaceManifestV2 result;
    result.manifest_sha256_ = std::string(expected_manifest_sha256);
    const auto parse_line = [&](std::string item, std::string& current_section,
            bool& version, bool& root, VerifiedNamespaceManifestV2& parsed,
            std::set<std::string>& packages, std::set<std::string>& files) {
        item = namespace_v2_trim(item);
        if (item.empty()) return;
        if (item.front() == '[' && item.back() == ']') {
            const auto heading = item.substr(1, item.size() - 2);
            if (heading == "project") current_section = heading;
            else if (heading.rfind("packages.", 0) == 0
                    && namespace_v2_name(std::string_view(heading).substr(9))) {
                current_section = heading;
                if (!packages.insert(heading.substr(9)).second)
                    throw std::invalid_argument("duplicate v2 package table");
            } else throw std::invalid_argument("v2 namespace manifest has unsupported TOML section");
            return;
        }
        const auto pos = item.find('=');
        if (pos == std::string::npos) throw std::invalid_argument("v2 manifest expected an assignment");
        const auto key = namespace_v2_trim(item.substr(0, pos));
        const auto value = namespace_v2_trim(item.substr(pos + 1));
        if (current_section.empty() && key == "schema_version" && value == "\"2\"" && !version) {
            version = true;
        } else if (current_section == "project" && key == "root" && value == "\".\"" && !root) {
            root = true;
        } else if (current_section.rfind("packages.", 0) == 0 && key == "files"
                && parsed.package_files_.count(current_section.substr(9)) == 0) {
            auto values = namespace_v2_parse_files(value);
            for (const auto& file : values)
                if (!files.insert(file).second)
                    throw std::invalid_argument("v2 source belongs to multiple packages");
            parsed.package_files_.emplace(current_section.substr(9), std::move(values));
        } else throw std::invalid_argument("v2 namespace manifest has unsupported or duplicate key");
    };
    std::size_t start = 0;
    while (start < manifest_bytes.size()) {
        const auto end = manifest_bytes.find('\n', start);
        const auto len = end == manifest_bytes.npos ? manifest_bytes.size() - start : end - start;
        std::string line = namespace_v2_trim(namespace_v2_strip_comment(
            std::string(manifest_bytes.substr(start, len))));
        if (!pending.empty()) {
            pending += " " + line;
            if (line.find(']') != std::string::npos) {
                parse_line(std::move(pending), section, saw_schema, saw_project_root,
                           result, seen_packages, seen_files);
                pending.clear();
            }
        } else if (!line.empty()) {
            if (line.rfind("files", 0) == 0 && line.find('[') != line.npos
                    && line.find(']') == line.npos) pending = line;
            else parse_line(std::move(line), section, saw_schema, saw_project_root,
                            result, seen_packages, seen_files);
        }
        if (end == manifest_bytes.npos) break;
        start = end + 1;
    }
    if (!pending.empty() || !saw_schema || !saw_project_root || seen_packages.empty()
            || result.package_files_.size() != seen_packages.size())
        throw std::invalid_argument("v2 namespace manifest is incomplete or unsupported");
    if (seen_files.size() != admitted_sources.size())
        throw std::invalid_argument("v2 authenticated source inventory is incomplete");
    std::string source_commitment;
    std::size_t total = 0;
    for (const auto& file : seen_files) {
        const auto it = admitted_sources.find(file);
        if (it == admitted_sources.end())
            throw std::invalid_argument("v2 authenticated source is absent");
        const auto bytes = it->second.size();
        if (bytes > 8 * 1024 * 1024 || total > 64 * 1024 * 1024 - bytes)
            throw std::invalid_argument("v2 authenticated sources exceed byte budget");
        total += bytes;
        const auto digest = sha256(it->second);
        if (!namespace_v2_digest(digest))
            throw std::invalid_argument("v2 trusted SHA-256 hasher produced invalid digest");
        source_commitment += file + "\n" + digest + "\n";
    }
    result.sources_sha256_ = sha256(source_commitment);
    if (result.sources_sha256_ != expected_sources_sha256)
        throw std::invalid_argument("v2 authenticated source inventory SHA-256 mismatch");
    return result;
}

} // namespace oreslang::frontend
