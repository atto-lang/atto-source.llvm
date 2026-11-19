#pragma once

// Integrity-only admission: no parser AST, LLVM IR, runtime capability or actor execution.
#include "oreslang/frontend/SourceInput.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace oreslang::frontend {

inline std::uint32_t source_set_rol(std::uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

inline std::string source_set_git_blob_sha(std::string_view source) {
    // Git SHA-1 identifies repo objects; it is not a signature.
    std::string bytes = "blob " + std::to_string(source.size());
    bytes.push_back('\0');
    bytes.append(source.data(), source.size());
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8u;
    bytes.push_back(static_cast<char>(0x80));
    while (bytes.size() % 64 != 56) bytes.push_back('\0');
    for (int i = 7; i >= 0; --i)
        bytes.push_back(static_cast<char>((bits >> (i * 8)) & 0xffu));
    std::array<std::uint32_t, 5> h{{0x67452301u, 0xefcdab89u, 0x98badcfeu,
                                     0x10325476u, 0xc3d2e1f0u}};
    for (std::size_t pos = 0; pos < bytes.size(); pos += 64) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t p = pos + 4 * i;
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p + 2])) << 8) |
                   static_cast<unsigned char>(bytes[p + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i)
            w[i] = source_set_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            const auto next = source_set_rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = source_set_rol(b, 30); b = a; a = next;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(40);
    for (auto v : h)
        for (int i = 7; i >= 0; --i)
            digest.push_back(hex[(v >> (i * 4)) & 0xfu]);
    return digest;
}

inline bool source_set_hex_sha(std::string_view sha) {
    if (sha.size() != 40) return false;
    for (char c : sha)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool source_set_identifier(std::string_view name) {
    if (name.empty() || name.size() > 80) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
            (i > 0 && c >= '0' && c <= '9')) continue;
        return false;
    }
    return true;
}

inline bool source_set_path(std::string_view path) {
    if (path.empty() || path.size() > 256 || path.front() == '/' ||
        path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string_view::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
                return false;
        if (end == std::string_view::npos) return true;
        start = end + 1;
    }
    return false;
}

struct ActorSourceSetReceipt {
    std::string name_space;
    std::string actor;
    std::string contract;
    std::size_t unit_count = 0;
    std::size_t source_bytes = 0;
};

inline ActorSourceSetReceipt verify_actor_source_set_only(
        const std::filesystem::path& manifest_path, std::string_view expected_git_sha) {
    namespace fs = std::filesystem;
    if (!source_set_hex_sha(expected_git_sha))
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: expected trusted lowercase Git SHA");
    if (fs::is_symlink(fs::symlink_status(manifest_path)))
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: manifest symlink");
    const std::string manifest = read_regular_source(manifest_path.string().c_str(),
        16384u, "ACTOR_SOURCE_SET_REJECT: manifest byte budget");
    require_valid_source_utf8(manifest);
    if (source_set_git_blob_sha(manifest) != expected_git_sha)
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: untrusted manifest Git blob");
    if (manifest.empty() || manifest.back() != '\n' || manifest.find('\r') != std::string::npos)
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: noncanonical manifest lines");
    const fs::path root = fs::canonical(manifest_path).parent_path();
    std::istringstream lines(manifest);
    std::string line;
    auto next = [&]() {
        if (!std::getline(lines, line))
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: incomplete manifest");
    };
    next();
    if (line != "atto-declared-actor-source-set-v1")
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: unsupported manifest schema");
    ActorSourceSetReceipt receipt;
    next();
    if (line.rfind("namespace ", 0) != 0 || !source_set_identifier(line.substr(10)))
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: namespace identity");
    receipt.name_space = line.substr(10);
    next();
    if (line.rfind("actor ", 0) != 0 || !source_set_identifier(line.substr(6)))
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: actor identity");
    receipt.actor = line.substr(6);
    next();
    if (line.rfind("contract ", 0) != 0 || !source_set_identifier(line.substr(9)))
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: contract identity");
    receipt.contract = line.substr(9);
    const std::array<std::string, 7> roles{{"package", "actor", "contract", "main",
                                            "method", "receive", "state"}};
    std::unordered_set<std::string> seen_roles, seen_paths;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string unit, role, relative, sha, extra;
        if (!(fields >> unit >> role >> relative >> sha) || (fields >> extra) ||
            line != "unit " + role + " " + relative + " " + sha || unit != "unit" ||
            !source_set_hex_sha(sha) || !source_set_path(relative))
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: noncanonical unit entry");
        if (std::find(roles.begin(), roles.end(), role) == roles.end() ||
            !seen_roles.insert(role).second || !seen_paths.insert(relative).second)
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: duplicate or unknown unit");
        const bool package = role == "package";
        if ((package && relative != ".atto.pkg.toml") ||
            (!package && (relative.size() < 5 ||
                          relative.substr(relative.size() - 5) != ".atto")))
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: unit extension");
        fs::path file = root;
        std::istringstream parts(relative);
        std::string segment;
        while (std::getline(parts, segment, '/')) {
            file /= segment;
            if (fs::is_symlink(fs::symlink_status(file)))
                throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: source symlink");
        }
        if (!fs::is_regular_file(file))
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: missing regular source unit");
        const std::string data = read_regular_source(file.string().c_str(),
            131072u, "ACTOR_SOURCE_SET_REJECT: unit byte budget");
        require_valid_source_utf8(data);
        if (data.empty() || source_set_git_blob_sha(data) != sha)
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: source Git blob mismatch");
        if (receipt.source_bytes > 1048576u - data.size())
            throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: aggregate byte budget");
        receipt.source_bytes += data.size();
        ++receipt.unit_count;
    }
    if (seen_roles.size() != roles.size())
        throw std::runtime_error("ACTOR_SOURCE_SET_REJECT: incomplete seven-unit source set");
    return receipt; // NO AST, LLVM IR, or actor runtime authority.
}

} // namespace oreslang::frontend
