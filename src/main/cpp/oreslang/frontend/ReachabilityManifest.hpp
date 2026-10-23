#pragma once

#include "oreslang/frontend/SemanticReachability.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang {

struct OresReachabilityManifestV1 {
    std::string build_kind;
    std::vector<std::string> roots;
    std::vector<std::string> retained_symbols;
    std::vector<std::string> removed_symbols;
    std::map<std::string, std::string> compile_time_constants_json;
};

namespace reachability_manifest_detail {

inline std::string semantic_module_name(const Program& program) {
    return program.module.empty() ? "__root__" : program.module;
}

inline void validate_utf8_symbol(const std::string& value) {
    if (value.empty())
        throw std::runtime_error("reachability symbol cannot be empty");

    std::size_t code_points = 0;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        if (lead <= 0x1f || lead == 0x7f)
            throw std::runtime_error(
                "reachability symbol contains forbidden control character");

        std::size_t continuation_count = 0;
        unsigned char min_first = 0x80;
        unsigned char max_first = 0xbf;
        if (lead <= 0x7f) {
            continuation_count = 0;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation_count = 2;
            if (lead == 0xe0) min_first = 0xa0;
            if (lead == 0xed) max_first = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation_count = 3;
            if (lead == 0xf0) min_first = 0x90;
            if (lead == 0xf4) max_first = 0x8f;
        } else {
            throw std::runtime_error(
                "reachability symbol is not valid UTF-8");
        }

        if (value.size() - i <= continuation_count)
            throw std::runtime_error(
                "reachability symbol is truncated UTF-8");

        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto byte =
                static_cast<unsigned char>(value[i + j]);
            if (byte < (j == 1 ? min_first : 0x80) ||
                    byte > (j == 1 ? max_first : 0xbf))
                throw std::runtime_error(
                    "reachability symbol is not valid UTF-8");
        }

        ++code_points;
        if (code_points > 1024)
            throw std::runtime_error(
                "reachability symbol exceeds 1024 Unicode scalar values");
        i += continuation_count + 1;
    }
}

inline std::string semantic_symbol(
        const Program& program,
        const SemanticDeclarationId& id,
        const std::map<std::string, std::size_t>& function_family_sizes) {
    if (id.kind == SemanticDeclarationKind::Function) {
        const auto family = function_family_sizes.find(id.name);
        if (family == function_family_sizes.end())
            throw std::runtime_error(
                "reachability manifest references unknown function family: " +
                id.name);
        if (family->second != 1)
            throw std::runtime_error(
                "ores-reachability/1 has no shared overload symbol identity yet: " +
                id.name);
    }

    const std::string symbol =
        semantic_module_name(program) + "." + id.name;
    validate_utf8_symbol(symbol);
    return symbol;
}

inline void require_sorted_unique(
        const std::vector<std::string>& values,
        const char* label) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (!(values[i - 1] < values[i]))
            throw std::runtime_error(
                std::string(label) +
                " must be sorted and unique");
    }
}

inline std::string json_quote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(ch >> 4) & 0x0f]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
    return out;
}

inline void append_json_array(
        std::string& out,
        const std::vector<std::string>& values) {
    out.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += json_quote(values[i]);
    }
    out.push_back(']');
}

} // namespace reachability_manifest_detail

inline OresReachabilityManifestV1 make_reachability_manifest_v1(
        const Program& program,
        const SemanticReachabilityResult& reachability,
        SemanticBuildKind build_kind) {
    using namespace reachability_manifest_detail;

    std::map<std::string, std::size_t> function_family_sizes;
    for (const auto& function : program.functions)
        ++function_family_sizes[function.name];

    OresReachabilityManifestV1 manifest;
    manifest.build_kind =
        build_kind == SemanticBuildKind::Library
            ? "library"
            : "executable";

    std::map<SemanticDeclarationId, std::string> encoded;
    auto encode = [&](const SemanticDeclarationId& id) -> const std::string& {
        const auto existing = encoded.find(id);
        if (existing != encoded.end()) return existing->second;
        const std::string symbol =
            semantic_symbol(program, id, function_family_sizes);
        for (const auto& [other_id, other_symbol] : encoded) {
            if (other_id != id && other_symbol == symbol)
                throw std::runtime_error(
                    "semantic reachability symbol collision: " + symbol);
        }
        return encoded.emplace(id, symbol).first->second;
    };

    for (const auto& id : reachability.roots)
        manifest.roots.push_back(encode(id));
    for (const auto& id : reachability.retained)
        manifest.retained_symbols.push_back(encode(id));
    for (const auto& id : reachability.removed)
        manifest.removed_symbols.push_back(encode(id));

    auto sort_unique = [](std::vector<std::string>& values) {
        std::sort(values.begin(), values.end());
        if (std::adjacent_find(values.begin(), values.end()) != values.end())
            throw std::runtime_error(
                "duplicate semantic symbol in reachability manifest");
    };
    sort_unique(manifest.roots);
    sort_unique(manifest.retained_symbols);
    sort_unique(manifest.removed_symbols);

    require_sorted_unique(manifest.roots, "reachability roots");
    require_sorted_unique(
        manifest.retained_symbols, "retained symbols");
    require_sorted_unique(
        manifest.removed_symbols, "removed symbols");

    for (const auto& root : manifest.roots) {
        if (!std::binary_search(
                manifest.retained_symbols.begin(),
                manifest.retained_symbols.end(),
                root))
            throw std::runtime_error(
                "reachability root is not retained: " + root);
    }

    for (const auto& symbol : manifest.retained_symbols) {
        if (std::binary_search(
                manifest.removed_symbols.begin(),
                manifest.removed_symbols.end(),
                symbol))
            throw std::runtime_error(
                "reachability symbol appears in retained and removed sets: " +
                symbol);
    }

    return manifest;
}

inline std::string canonical_reachability_manifest_json(
        const OresReachabilityManifestV1& manifest) {
    using namespace reachability_manifest_detail;

    if (manifest.build_kind != "executable" &&
            manifest.build_kind != "library")
        throw std::runtime_error(
            "invalid ores-reachability/1 build kind");

    require_sorted_unique(manifest.roots, "reachability roots");
    require_sorted_unique(
        manifest.retained_symbols, "retained symbols");
    require_sorted_unique(
        manifest.removed_symbols, "removed symbols");

    std::string out =
        "{\"format\":\"ores-reachability/1\",\"buildKind\":" +
        json_quote(manifest.build_kind) +
        ",\"roots\":";
    append_json_array(out, manifest.roots);
    out += ",\"retainedSymbols\":";
    append_json_array(out, manifest.retained_symbols);
    out += ",\"removedSymbols\":";
    append_json_array(out, manifest.removed_symbols);
    out += ",\"compileTimeConstants\":{";

    bool first = true;
    for (const auto& [name, encoded_json] :
            manifest.compile_time_constants_json) {
        validate_utf8_symbol(name);
        if (!first) out.push_back(',');
        first = false;
        out += json_quote(name);
        out.push_back(':');
        out += encoded_json;
    }
    out += "}}";
    return out;
}

} // namespace oreslang
