#include "oreslang/frontend/PackageManifestNamespaceInventory.hpp"

#include "oreslang/frontend/PackageNamespaceGraph.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oreslang {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kMaxManifestBytes = 64u * 1024u;
constexpr std::size_t kMaxManifestPathBytes = 4096;
constexpr std::size_t kMaxPackageGroups = 128;
constexpr std::size_t kMaxPackageFiles = 128;
constexpr std::size_t kMaxPackageInstanceBytes = 1024;

[[noreturn]] void fail(const std::string& reason) {
    throw std::runtime_error("verified package namespace inventory: " + reason);
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::string strip_comment(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\\')
            fail("manifest string escapes require a full TOML parser");
        if (ch == '"') quoted = !quoted;
        if (ch == '#' && !quoted) return line.substr(0, i);
    }
    if (quoted) fail("unterminated manifest string");
    return line;
}

bool array_complete(const std::string& text) {
    bool quoted = false;
    int depth = 0;
    for (char ch : text) {
        if (ch == '\\')
            fail("manifest string escapes require a full TOML parser");
        if (ch == '"') quoted = !quoted;
        if (quoted) continue;
        if (ch == '[') ++depth;
        else if (ch == ']') --depth;
        if (depth < 0) fail("malformed manifest array");
    }
    if (quoted) fail("unterminated manifest string");
    return depth == 0;
}

std::vector<std::string> logical_lines(const std::string& bytes) {
    if (bytes.size() > kMaxManifestBytes)
        fail("package manifest exceeds 64 KiB");
    if (bytes.find('\0') != std::string::npos)
        fail("package manifest contains NUL");
    if (bytes.find("\"\"\"") != std::string::npos ||
            bytes.find("'''") != std::string::npos)
        fail("package manifest multiline strings require a full TOML parser");
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r' && (i + 1 == bytes.size() || bytes[i + 1] != '\n'))
            fail("package manifest contains bare CR line ending");
    }

    std::istringstream stream(bytes);
    std::vector<std::string> result;
    std::string pending;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(strip_comment(line));
        if (line.empty()) continue;
        if (!pending.empty()) {
            pending += " ";
            pending += line;
            if (array_complete(pending)) {
                result.push_back(std::move(pending));
                pending.clear();
            }
            continue;
        }
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string value = trim(line.substr(eq + 1));
            if (!value.empty() && value.front() == '[' && !array_complete(value)) {
                pending = line;
                continue;
            }
        }
        result.push_back(std::move(line));
    }
    if (!pending.empty()) fail("unterminated manifest array");
    return result;
}

std::string parse_string(const std::string& value, const char* field) {
    const std::string text = trim(value);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"')
        fail(std::string(field) + " must be a basic quoted string");
    std::string result = text.substr(1, text.size() - 2);
    if (result.find('"') != std::string::npos ||
            result.find('\\') != std::string::npos)
        fail(std::string(field) + " uses unsupported TOML string escaping");
    for (unsigned char ch : result) {
        if (ch < 32 || ch == 127)
            fail(std::string(field) + " contains control bytes");
    }
    return result;
}

std::vector<std::string> parse_string_array(
        const std::string& value, const char* field) {
    const std::string text = trim(value);
    if (text.size() < 2 || text.front() != '[' || text.back() != ']')
        fail(std::string(field) + " must be an array of strings");
    std::vector<std::string> result;
    std::size_t at = 1;
    while (at + 1 < text.size()) {
        while (at + 1 < text.size() &&
                std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        if (at + 1 >= text.size() || text[at] == ']') break;
        if (text[at] != '"')
            fail(std::string(field) + " must contain only quoted strings");
        const std::size_t start = ++at;
        while (at < text.size() && text[at] != '"') {
            if (text[at] == '\\')
                fail(std::string(field) + " string escapes require full TOML parser");
            if (static_cast<unsigned char>(text[at]) < 32)
                fail(std::string(field) + " contains control bytes");
            ++at;
        }
        if (at >= text.size()) fail(std::string(field) + " has unterminated string");
        result.push_back(text.substr(start, at - start));
        ++at;
        while (at < text.size() &&
                std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        if (at >= text.size()) fail(std::string(field) + " has malformed array");
        if (text[at] == ']') {
            if (at + 1 != text.size())
                fail(std::string(field) + " has trailing material after array");
            break;
        }
        if (text[at] != ',')
            fail(std::string(field) + " requires comma-separated strings");
        ++at;
        while (at < text.size() &&
                std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        if (at < text.size() && text[at] == ']') break;
    }
    return result;
}

bool valid_project_name(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'
            || ch == '.';
    });
}

bool valid_package_key(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    const auto start = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || ch == '_';
    };
    if (!start(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin() + 1, value.end(), [&](unsigned char ch) {
        return start(ch) || (ch >= '0' && ch <= '9') || ch == '-';
    });
}

bool valid_package_instance_id(const std::string& value) {
    if (value.empty() || value.size() > kMaxPackageInstanceBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 32 && ch != 127;
    });
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool relative_path_spelling_valid(
        std::string_view raw, bool allow_dot, bool require_ores) {
    if (allow_dot && raw == ".") return true;
    if (raw.empty() || raw.size() > kMaxManifestPathBytes
            || raw.front() == '/' || raw.find('\\') != std::string_view::npos
            || raw.find(':') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto slash = raw.find('/', start);
        const auto end = slash == std::string_view::npos ? raw.size() : slash;
        const auto part = raw.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char ch : part) {
            const bool okay = (ch >= 'a' && ch <= 'z')
                || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                || ch == '_' || ch == '-' || ch == '.';
            if (!okay) return false;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return !require_ores
        || (raw.size() > 5 && raw.substr(raw.size() - 5) == ".ores");
}

bool within(const fs::path& root, const fs::path& path) {
    auto a = root.begin();
    auto b = path.begin();
    for (; a != root.end(); ++a, ++b) {
        if (b == path.end() || *a != *b) return false;
    }
    return true;
}

bool same_physical_file(
        const fs::path& left, const fs::path& right, const char* context) {
    std::error_code error;
    const bool equivalent = fs::equivalent(left, right, error);
    if (error)
        fail(std::string("cannot compare ") + context + " physical identities");
    return equivalent;
}

bool marker_exists(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) return false;
        fail("cannot safely inspect nested package marker");
    }
    return status.type() != fs::file_type::not_found;
}

void reject_nested_markers(
        const fs::path& root, const fs::path& path, const char* context) {
    if (!within(root, path))
        fail(std::string(context) + " escapes physical project root");
    for (fs::path parent = path; parent != root; parent = parent.parent_path()) {
        if (parent.empty() || parent == parent.root_path())
            fail(std::string(context) + " escapes physical project root");
        if (marker_exists(parent / ".ores.pkg.toml")
                || marker_exists(parent / ".oreslangc.cfg.toml"))
            fail(std::string(context) + " crosses nested package boundary");
    }
}

fs::path canonical_regular_manifest(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || status.type() != fs::file_type::regular)
        fail("modern .ores.pkg.toml must be a regular non-symlink file");
    const auto canonical = fs::canonical(path, error);
    if (error) fail("cannot canonicalize package manifest");
    return canonical;
}

fs::path canonical_regular_source(
        const fs::path& project_root,
        const fs::path& spelled,
        const std::string& relative) {
    std::error_code error;
    const auto status = fs::symlink_status(spelled, error);
    if (error || (status.type() != fs::file_type::regular
                  && status.type() != fs::file_type::symlink))
        fail("manifest package member is missing or not a regular source");
    const auto physical = fs::canonical(spelled, error);
    if (error || !fs::is_regular_file(physical, error) || error)
        fail("cannot canonicalize manifest package source '" + relative + "'");
    if (!within(project_root, physical))
        fail("manifest package source escapes physical project root");
    reject_nested_markers(
        project_root, spelled.parent_path(),
        "manifest package source spelling");
    reject_nested_markers(
        project_root, physical.parent_path(),
        "manifest package source target");
    return physical;
}

std::string source_unit_id(
        const std::string& package_instance_id,
        const std::string& manifest_relative_path) {
    return std::to_string(package_instance_id.size()) + ":"
        + package_instance_id + ":" + manifest_relative_path;
}

struct PackageSelection {
    std::string package_instance_id;
    const VerifiedPackageManifestV2* manifest = nullptr;
    const VerifiedLockedPackageInstance* lock_entry = nullptr;
};

const VerifiedPackageManifestV2* find_manifest(
        const fs::path& exact,
        const VerifiedPackageManifestV2& root,
        const std::vector<VerifiedPackageManifestV2>& dependencies) {
    if (same_physical_file(
            root.manifest_path, exact, "manifest authority"))
        return &root;
    const VerifiedPackageManifestV2* found = nullptr;
    for (const auto& manifest : dependencies) {
        if (!same_physical_file(
                manifest.manifest_path, exact, "manifest authority"))
            continue;
        if (found) fail("duplicate dependency manifest authority");
        found = &manifest;
    }
    return found;
}

} // namespace

std::string namespace_inventory_sha256(std::string_view bytes) {
    static constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    auto rotate = [](std::uint32_t value, unsigned count) {
        return (value >> count) | (value << (32u - count));
    };

    std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(data.size()) * 8u;
    data.push_back(0x80u);
    while (data.size() % 64u != 56u) data.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<std::uint8_t>(bit_length >> shift));

    std::array<std::uint32_t, 8> hash = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    for (std::size_t block = 0; block < data.size(); block += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto at = block + i * 4;
            words[i] = (static_cast<std::uint32_t>(data[at]) << 24)
                | (static_cast<std::uint32_t>(data[at + 1]) << 16)
                | (static_cast<std::uint32_t>(data[at + 2]) << 8)
                | static_cast<std::uint32_t>(data[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotate(words[i - 15], 7)
                ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = rotate(words[i - 2], 17)
                ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a=hash[0],b=hash[1],c=hash[2],d=hash[3];
        auto e=hash[4],f=hash[5],g=hash[6],h=hash[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = rotate(e,6) ^ rotate(e,11) ^ rotate(e,25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto t1 = h + s1 + choose + constants[i] + words[i];
            const auto s0 = rotate(a,2) ^ rotate(a,13) ^ rotate(a,22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        hash[0]+=a;hash[1]+=b;hash[2]+=c;hash[3]+=d;
        hash[4]+=e;hash[5]+=f;hash[6]+=g;hash[7]+=h;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto word : hash) out << std::setw(8) << word;
    return out.str();
}

VerifiedPackageManifestV2 load_verified_package_manifest_v2(
        const fs::path& manifest_path,
        const std::function<std::string(const fs::path&)>& read_manifest) {
    if (!read_manifest) fail("manifest reader is required");
    const fs::path manifest = canonical_regular_manifest(manifest_path);
    const std::string bytes = read_manifest(manifest);
    const auto lines = logical_lines(bytes);

    std::string section;
    std::size_t schema_count = 0;
    std::string project_name;
    std::string project_root_text = ".";
    bool project_root_seen = false;
    std::vector<std::string> source_roots_text{"."};
    bool source_roots_seen = false;
    std::map<std::string, std::vector<std::string>> package_paths;
    std::set<std::string> package_files_seen;
    std::set<std::string> declared_tables;
    bool project_version_seen = false;
    bool source_import_paths_seen = false;
    bool entrypoint_main_seen = false;
    bool build_profile_seen = false;

    for (const auto& line : lines) {
        if (line.front() == '[') {
            if (line.back() != ']'
                    || line.find('[', 1) != std::string::npos
                    || line.find(']') != line.size() - 1)
                fail("malformed package manifest table header");
            section = trim(line.substr(1, line.size() - 2));
            if (!declared_tables.insert(section).second)
                fail("duplicate package manifest table [" + section + "]");
            if (section == "project" || section == "source"
                    || section == "entrypoints" || section == "build")
                continue;
            if (section.rfind("packages.", 0) == 0) {
                const std::string key = section.substr(9);
                if (!valid_package_key(key))
                    fail("invalid [packages.<key>] table");
                if (package_paths.size() >= kMaxPackageGroups
                        && !package_paths.count(key))
                    fail("package manifest exceeds 128 explicit package groups");
                package_paths.try_emplace(key);
                continue;
            }
            fail("unsupported package manifest table [" + section + "]");
        }

        const auto equal = line.find('=');
        if (equal == std::string::npos)
            fail("package manifest assignment requires '='");
        const std::string key = trim(line.substr(0, equal));
        const std::string value = trim(line.substr(equal + 1));
        if (key.empty() || value.empty())
            fail("empty package manifest assignment");

        if (section.empty()) {
            if (key != "schema_version")
                fail("unsupported top-level package manifest key '" + key + "'");
            if (++schema_count != 1 || parse_string(value, "schema_version") != "2")
                fail("package namespace inventory requires schema_version = \"2\"");
            continue;
        }

        if (section == "project") {
            if (key == "name") {
                if (!project_name.empty())
                    fail("duplicate project.name");
                project_name = parse_string(value, "project.name");
                if (!valid_project_name(project_name))
                    fail("invalid project.name");
            } else if (key == "root") {
                if (project_root_seen) fail("duplicate project.root");
                project_root_seen = true;
                project_root_text = parse_string(value, "project.root");
                if (!relative_path_spelling_valid(
                        project_root_text, true, false))
                    fail("project.root must be canonical relative path within manifest directory");
            } else if (key == "version") {
                if (project_version_seen) fail("duplicate project.version");
                project_version_seen = true;
                (void)parse_string(value, "project.version");
            } else {
                fail("unsupported [project] key '" + key + "'");
            }
            continue;
        }

        if (section == "source") {
            if (key == "roots") {
                if (source_roots_seen) fail("duplicate source.roots");
                source_roots_seen = true;
                source_roots_text = parse_string_array(value, "source.roots");
                for (const auto& root : source_roots_text) {
                    if (!relative_path_spelling_valid(root, true, false))
                        fail("source.roots contains noncanonical relative path");
                }
            } else if (key == "import_paths") {
                if (source_import_paths_seen)
                    fail("duplicate source.import_paths");
                source_import_paths_seen = true;
                const auto ignored =
                    parse_string_array(value, "source.import_paths");
                for (const auto& path : ignored) {
                    if (!relative_path_spelling_valid(path, true, false))
                        fail("source.import_paths contains noncanonical relative path");
                }
            } else {
                fail("unsupported [source] key '" + key + "'");
            }
            continue;
        }

        if (section == "entrypoints") {
            if (key != "main")
                fail("unsupported [entrypoints] key '" + key + "'");
            if (entrypoint_main_seen) fail("duplicate entrypoints.main");
            entrypoint_main_seen = true;
            const auto entry = parse_string(value, "entrypoints.main");
            if (!relative_path_spelling_valid(entry, false, true))
                fail("entrypoints.main must be canonical relative .ores path");
            continue;
        }

        if (section == "build") {
            if (key != "library_profile")
                fail("unsupported [build] key '" + key + "'");
            if (build_profile_seen) fail("duplicate build.library_profile");
            build_profile_seen = true;
            const auto profile = parse_string(value, "build.library_profile");
            if (profile != "core" && profile != "std")
                fail("build.library_profile must be core or std");
            continue;
        }

        if (section.rfind("packages.", 0) == 0) {
            const std::string group = section.substr(9);
            if (key != "files")
                fail("unsupported package group key '" + key + "'");
            if (!package_files_seen.insert(group).second)
                fail("duplicate package files assignment for '" + group + "'");
            auto files = parse_string_array(value, "packages.files");
            if (files.size() > kMaxPackageFiles)
                fail("package group exceeds 128 explicit source files");
            for (const auto& path : files) {
                if (!relative_path_spelling_valid(path, false, true))
                    fail("package source path must be canonical relative .ores path");
            }
            package_paths[group] = std::move(files);
            continue;
        }

        fail("manifest assignment outside recognized table");
    }

    if (schema_count != 1)
        fail("package namespace inventory requires schema_version = \"2\"");
    if (project_name.empty())
        fail("schema v2 package namespace inventory requires project.name");
    for (const auto& [group, files] : package_paths) {
        (void)files;
        if (!package_files_seen.count(group))
            fail("package group '" + group + "' has no files assignment");
    }

    const fs::path manifest_dir = manifest.parent_path();
    std::error_code error;
    const fs::path physical_manifest_dir = fs::canonical(manifest_dir, error);
    if (error) fail("cannot canonicalize manifest directory");

    fs::path spelled_root = project_root_text == "."
        ? manifest_dir : manifest_dir / fs::path(project_root_text);
    const fs::path physical_root = fs::canonical(spelled_root, error);
    if (error || !fs::is_directory(physical_root, error) || error)
        fail("project.root does not resolve to directory");
    if (!within(physical_manifest_dir, physical_root))
        fail("project.root escapes physical manifest directory");
    if (physical_root != physical_manifest_dir) {
        reject_nested_markers(
            physical_manifest_dir, physical_root,
            "configured project.root");
    }

    VerifiedPackageManifestV2 result;
    result.manifest_path = manifest;
    result.project_root = physical_root;
    result.project_name = project_name;
    result.manifest_sha256 = namespace_inventory_sha256(bytes);

    std::set<std::string> root_ids;
    for (const auto& root_text : source_roots_text) {
        fs::path root = root_text == "."
            ? physical_root : physical_root / fs::path(root_text);
        root = fs::canonical(root, error);
        if (error || !fs::is_directory(root, error) || error)
            fail("source.roots entry does not resolve to directory");
        if (!within(physical_root, root))
            fail("source.roots entry escapes project.root");
        reject_nested_markers(physical_root, root, "source.roots entry");
        if (root_ids.insert(root.string()).second)
            result.source_roots.push_back(root);
    }

    std::vector<fs::path> all_members;
    for (const auto& [group, paths] : package_paths) {
        auto& members = result.packages[group];
        std::set<std::string> spelling_seen;
        for (const auto& relative : paths) {
            if (!spelling_seen.insert(relative).second)
                fail("duplicate package source spelling '" + relative + "'");
            const fs::path spelled = physical_root / fs::path(relative);
            const fs::path physical = canonical_regular_source(
                physical_root, spelled, relative);
            for (const auto& previous : all_members) {
                std::error_code equivalent_error;
                const bool equivalent =
                    fs::equivalent(previous, physical, equivalent_error);
                if (equivalent_error)
                    fail("cannot compare physical package source identities");
                if (equivalent)
                    fail("source file belongs to more than one explicit package group");
            }
            all_members.push_back(physical);
            members.push_back({relative, physical});
        }
    }
    return result;
}

VerifiedPackageManifestV2 discover_verified_package_manifest_v2(
        const fs::path& entry_source,
        const std::function<std::string(const fs::path&)>& read_manifest) {
    if (!read_manifest) fail("manifest reader is required");
    std::error_code error;
    const fs::path physical_entry = fs::canonical(entry_source, error);
    if (error || !fs::is_regular_file(physical_entry, error) || error)
        fail("cannot discover package authority from missing/non-regular entry source");

    for (fs::path cursor = physical_entry.parent_path(); !cursor.empty();) {
        const fs::path modern = cursor / ".ores.pkg.toml";
        const fs::path legacy = cursor / ".oreslangc.cfg.toml";
        const bool modern_exists = marker_exists(modern);
        const bool legacy_exists = marker_exists(legacy);

        if (modern_exists)
            return load_verified_package_manifest_v2(modern, read_manifest);
        if (legacy_exists)
            fail("nearest package authority is legacy; schema-v2 namespace inventory requires .ores.pkg.toml");

        if (cursor == cursor.root_path()) break;
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    fail("schema-v2 namespace inventory requires nearest .ores.pkg.toml");
}

PinnedNamespaceProgramInput build_pinned_namespace_program_from_manifests(
        const fs::path& root_manifest_path,
        const std::vector<fs::path>& dependency_manifest_paths,
        const VerifiedPackageLockSnapshot& lock,
        const fs::path& entry_source,
        const std::function<std::string(const fs::path&)>& read_manifest,
        const std::function<std::string(const fs::path&)>& read_source) {
    if (!read_manifest) fail("manifest reader is required");
    if (!read_source) fail("source reader is required");

    const VerifiedPackageManifestV2 root_manifest =
        load_verified_package_manifest_v2(root_manifest_path, read_manifest);
    std::vector<VerifiedPackageManifestV2> dependency_manifests;
    dependency_manifests.reserve(dependency_manifest_paths.size());
    for (const auto& path : dependency_manifest_paths) {
        auto manifest = load_verified_package_manifest_v2(path, read_manifest);
        if (same_physical_file(
                manifest.manifest_path, root_manifest.manifest_path,
                "root/dependency manifest"))
            fail("root manifest cannot also be supplied as dependency manifest");
        for (const auto& prior : dependency_manifests) {
            if (same_physical_file(
                    prior.manifest_path, manifest.manifest_path,
                    "dependency manifest"))
                fail("duplicate dependency manifest authority");
        }
        dependency_manifests.push_back(std::move(manifest));
    }
    if (!valid_package_instance_id(lock.root_package_instance_id))
        fail("invalid root PackageInstanceId");
    if (lock.package_instances.empty())
        fail("verified package lock contains no package instances");

    std::map<std::string, fs::path> physical_manifest_by_instance;
    struct ManifestGroupIdentity {
        fs::path manifest;
        std::string package_key;
    };
    std::vector<ManifestGroupIdentity> manifest_group_identities;
    for (const auto& [instance_id, entry] : lock.package_instances) {
        if (!valid_package_instance_id(instance_id))
            fail("invalid PackageInstanceId in lock snapshot");
        if (!valid_package_key(entry.package_key))
            fail("invalid locked package key");
        if (!valid_sha256(entry.manifest_sha256))
            fail("locked manifest SHA-256 must be lowercase 64-hex");
        const fs::path physical = canonical_regular_manifest(entry.manifest_path);
        physical_manifest_by_instance.emplace(instance_id, physical);
        for (const auto& claimed : manifest_group_identities) {
            if (claimed.package_key != entry.package_key) continue;
            std::error_code equivalent_error;
            const bool equivalent =
                fs::equivalent(claimed.manifest, physical, equivalent_error);
            if (equivalent_error)
                fail("cannot compare locked manifest physical identities");
            if (equivalent)
                fail("two PackageInstanceIds claim the same manifest package group");
        }
        manifest_group_identities.push_back({
            physical, entry.package_key});
        for (const auto& [relative, digest] : entry.source_sha256) {
            if (!relative_path_spelling_valid(relative, false, true)
                    || !valid_sha256(digest))
                fail("lock contains invalid source path or SHA-256");
        }
    }

    const auto root_lock = lock.package_instances.find(
        lock.root_package_instance_id);
    if (root_lock == lock.package_instances.end())
        fail("root PackageInstanceId is absent from lock snapshot");
    if (!same_physical_file(
            physical_manifest_by_instance.at(lock.root_package_instance_id),
            root_manifest.manifest_path, "root manifest"))
        fail("root manifest does not match locked manifest authority");

    std::set<std::string> selected_instances{
        lock.root_package_instance_id};
    for (const auto& [alias, target] : lock.dependency_aliases) {
        if (!frontend::valid_flat_namespace_name(alias))
            fail("invalid dependency alias '" + alias + "'");
        if (!valid_package_instance_id(target)
                || !lock.package_instances.count(target))
            fail("dependency alias targets unknown/unpinned package instance");
        selected_instances.insert(target);
    }

    std::map<std::string, PackageSelection> selections;
    for (const auto& instance_id : selected_instances) {
        const auto& entry = lock.package_instances.at(instance_id);
        const fs::path physical_manifest =
            physical_manifest_by_instance.at(instance_id);
        const auto* manifest = find_manifest(
            physical_manifest, root_manifest, dependency_manifests);
        if (!manifest)
            fail("locked dependency manifest was not supplied to compiler");
        if (manifest->manifest_sha256 != entry.manifest_sha256)
            fail("stale package lock: manifest SHA-256 mismatch");
        const auto package = manifest->packages.find(entry.package_key);
        if (package == manifest->packages.end())
            fail("locked package key is absent from manifest");
        if (entry.source_sha256.size() != package->second.size())
            fail("stale package lock: source membership set mismatch");
        for (const auto& member : package->second) {
            const auto digest = entry.source_sha256.find(
                member.manifest_relative_path);
            if (digest == entry.source_sha256.end())
                fail("stale package lock: manifest source is not pinned");
        }
        selections.emplace(instance_id, PackageSelection{
            instance_id, manifest, &entry});
    }

    // Dependency manifests supplied outside the exact root-alias closure are
    // refused so ambient caller state cannot enlarge the compilation graph.
    for (const auto& manifest : dependency_manifests) {
        bool selected = false;
        for (const auto& [instance_id, selection] : selections) {
            (void)instance_id;
            if (selection.manifest == &manifest) {
                selected = true;
                break;
            }
        }
        if (!selected)
            fail("unreferenced dependency manifest supplied outside lock closure");
    }

    std::error_code error;
    const fs::path physical_entry = fs::canonical(entry_source, error);
    if (error || !fs::is_regular_file(physical_entry, error) || error)
        fail("entry source is missing or not regular");

    const auto& root_selection =
        selections.at(lock.root_package_instance_id);
    const auto& root_members =
        root_selection.manifest->packages.at(
            root_selection.lock_entry->package_key);
    const VerifiedPackageSourceMember* entry_member = nullptr;
    for (const auto& member : root_members) {
        std::error_code equivalent_error;
        if (fs::equivalent(
                member.physical_path, physical_entry, equivalent_error)
                && !equivalent_error) {
            if (entry_member)
                fail("entry source has duplicate package membership");
            entry_member = &member;
        }
    }
    if (!entry_member)
        fail("entry source is not a member of the locked root package group");

    PinnedNamespaceProgramInput result;
    result.pinned_aliases[lock.root_package_instance_id] =
        lock.dependency_aliases;

    std::vector<std::string> ordered_instances(
        selected_instances.begin(), selected_instances.end());
    std::sort(ordered_instances.begin(), ordered_instances.end());
    std::vector<fs::path> physical_source_owners;
    std::size_t aggregate_bytes = 0;

    for (const auto& instance_id : ordered_instances) {
        const auto& selection = selections.at(instance_id);
        const auto& members =
            selection.manifest->packages.at(
                selection.lock_entry->package_key);
        std::vector<const VerifiedPackageSourceMember*> ordered_members;
        ordered_members.reserve(members.size());
        for (const auto& member : members) ordered_members.push_back(&member);
        std::sort(
            ordered_members.begin(), ordered_members.end(),
            [](const auto* left, const auto* right) {
                return left->manifest_relative_path
                    < right->manifest_relative_path;
            });

        for (const auto* member : ordered_members) {
            for (const auto& previous : physical_source_owners) {
                std::error_code equivalent_error;
                const bool equivalent = fs::equivalent(
                    previous, member->physical_path, equivalent_error);
                if (equivalent_error)
                    fail("cannot compare locked physical source identities");
                if (equivalent)
                    fail("one physical source is owned by multiple locked package instances");
            }
            physical_source_owners.push_back(member->physical_path);
            const auto size = fs::file_size(member->physical_path, error);
            if (error || size > 8u * 1024u * 1024u - aggregate_bytes)
                fail("locked namespace sources exceed 8 MiB aggregate budget");
            const std::string source = read_source(member->physical_path);
            aggregate_bytes += source.size();
            if (aggregate_bytes > 8u * 1024u * 1024u)
                fail("source reader exceeded 8 MiB aggregate budget");
            const auto expected = selection.lock_entry->source_sha256.find(
                member->manifest_relative_path);
            if (expected == selection.lock_entry->source_sha256.end()
                    || namespace_inventory_sha256(source) != expected->second)
                fail("stale package lock: source SHA-256 mismatch");

            const std::string unit = source_unit_id(
                instance_id, member->manifest_relative_path);
            if (member == entry_member
                    && instance_id == lock.root_package_instance_id)
                result.entry_source_unit_id = unit;
            result.sources.push_back({
                unit, instance_id, source});
        }
    }
    if (result.entry_source_unit_id.empty())
        fail("locked entry source identity was not materialized");
    return result;
}

} // namespace oreslang
