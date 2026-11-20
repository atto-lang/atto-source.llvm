#pragma once

// Independent LLVM-backend, fail-closed, source-controlled logging manifest
// intake. Strict ASCII JSON v1 subset: no escaped strings or dynamic payloads.
// This parser validates *bytes and exact inventory*, not source provenance,
// review approval, a linked guest artifact, W3C context, or actor/JIT identity.
#include "oreslang/runtime/SourceIdentityV1.hpp"
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {

struct CheckedSourceRoutineV1 {
    std::string symbol;
    std::size_t arity = 0;
    std::string routine_id;
};

struct CheckedLogSiteV1 {
    std::string symbol;
    std::size_t arity = 0;
    std::string message;
    std::string routine_id;
    std::string site_id;
};

struct CheckedSourceLogManifestV1 {
    std::string source_unit;
    std::vector<CheckedSourceRoutineV1> routines;
    std::vector<CheckedLogSiteV1> sites;
};

namespace checked_source_manifest_detail {

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {
        if (bytes_.size() > 131072)
            throw std::runtime_error("checked source log manifest exceeds 128 KiB");
    }
    void space() {
        while (at_ < bytes_.size() &&
               (bytes_[at_] == ' ' || bytes_[at_] == '\n' ||
                bytes_[at_] == '\r' || bytes_[at_] == '\t')) ++at_;
    }
    bool take(char c) {
        space();
        if (at_ < bytes_.size() && bytes_[at_] == c) { ++at_; return true; }
        return false;
    }
    void expect(char c) {
        if (!take(c))
            throw std::runtime_error("malformed checked source log manifest syntax");
    }
    std::string word() {
        expect('"');
        std::string result;
        while (at_ < bytes_.size() && bytes_[at_] != '"') {
            const auto c = static_cast<unsigned char>(bytes_[at_++]);
            // Deliberately no JSON escapes or non-ASCII in first static v1.
            // Such inputs are denied, never silently normalized or rekeyed.
            if (c < 32 || c > 126 || c == '\\' || result.size() == 1024)
                throw std::runtime_error("unsupported source log manifest string encoding");
            result.push_back(static_cast<char>(c));
        }
        if (result.empty() || at_ >= bytes_.size())
            throw std::runtime_error("empty or unterminated source log manifest string");
        ++at_;
        return result;
    }
    template<typename Field> void object(Field field) {
        expect('{');
        std::unordered_set<std::string> seen;
        if (take('}')) return;
        do {
            auto key = word();
            if (!seen.insert(key).second)
                throw std::runtime_error("duplicate checked source manifest JSON key");
            expect(':');
            field(key);
        } while (take(','));
        expect('}');
    }
    template<typename Item> void array(Item item) {
        expect('[');
        if (take(']')) return;
        std::size_t count = 0;
        do {
            if (++count > 4096)
                throw std::runtime_error("checked source manifest list exceeds 4096 entries");
            item();
        } while (take(','));
        expect(']');
    }
    void done() {
        space();
        if (at_ != bytes_.size())
            throw std::runtime_error("trailing unverified source manifest data");
    }
private:
    std::string_view bytes_;
    std::size_t at_ = 0;
};

inline bool valid_unit(std::string_view unit) {
    if (unit.empty() || unit.size() > 512 || unit.front() == '/' ||
        unit.find(':') != unit.npos || unit.find('\\') != unit.npos ||
        !(unit.size() >= 5 && (unit.substr(unit.size() - 5) == ".atto" ||
          unit.substr(unit.size() - 5) == ".ores"))) return false;
    for (std::size_t start = 0; start < unit.size();) {
        auto end = unit.find('/', start);
        if (end == unit.npos) end = unit.size();
        const auto part = unit.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

inline std::pair<std::string, std::size_t> selector(std::string_view signature) {
    const auto open = signature.find('(');
    const auto close = signature.find(')');
    if (open == signature.npos || close == signature.npos || open == 0 ||
        close < open || signature.substr(close) != "):int")
        throw std::runtime_error("unsupported native source callable signature");
    for (std::size_t i = 0; i < open; ++i) {
        const unsigned char c = static_cast<unsigned char>(signature[i]);
        if (!(std::isalpha(c) || c == '_' ||
              (i != 0 && std::isdigit(c))))
            throw std::runtime_error("invalid native source callable name");
    }
    std::size_t arity = 0;
    if (close != open + 1) {
        auto at = open + 1;
        do {
            if (signature.substr(at, 3) != "int")
                throw std::runtime_error("static source manifest needs scalar int parameters");
            at += 3;
            ++arity;
            if (at == close) break;
            if (signature[at] != ',' || at + 1 >= close)
                throw std::runtime_error("malformed source manifest parameter inventory");
            ++at;
        } while (arity <= 32);
        if (at != close || arity > 32)
            throw std::runtime_error("source manifest scalar arity exceeds 32");
    }
    return {std::string(signature.substr(0, open)), arity};
}

inline bool valid_message(std::string_view text) {
    if (text.empty() || text.size() > 96) return false;
    for (const unsigned char c : text)
        if (c < 32 || c == 127 || c > 126) return false;
    return true;
}

} // namespace checked_source_manifest_detail

inline CheckedSourceLogManifestV1 parse_checked_source_log_manifest_v1(
        std::string_view json, std::string_view expected_source_unit) {
    using namespace checked_source_manifest_detail;
    if (!valid_unit(expected_source_unit))
        throw std::runtime_error("native source-log identity requires canonical .atto/.ores path");
    Reader reader(json);
    CheckedSourceLogManifestV1 manifest;
    bool schema = false, unit = false, routines = false, sites = false;
    reader.object([&](const std::string& key) {
        if (key == "schema") {
            if (reader.word() != "atto.telemetry.source-ids.v1")
                throw std::runtime_error("unsupported native source-log registry schema");
            schema = true;
        } else if (key == "source_unit") {
            manifest.source_unit = reader.word();
            unit = true;
        } else if (key == "routines") {
            routines = true;
            reader.array([&] {
                std::string symbol, id;
                bool has_symbol = false, has_id = false;
                reader.object([&](const std::string& field) {
                    if (field == "symbol") { symbol = reader.word(); has_symbol = true; }
                    else if (field == "id") { id = reader.word(); has_id = true; }
                    else throw std::runtime_error("unexpected source-routine manifest field");
                });
                if (!has_symbol || !has_id)
                    throw std::runtime_error("incomplete source-routine manifest entry");
                const auto [name, arity] = selector(symbol);
                telemetry::SourceId parsed{};
                if (!telemetry::parse_source_id(id, telemetry::SourceKind::Routine, parsed))
                    throw std::runtime_error("invalid static source routine ID");
                manifest.routines.push_back({name, arity, id});
            });
        } else if (key == "sites") {
            sites = true;
            reader.array([&] {
                std::string symbol, message, routine, id;
                bool has_symbol = false, has_message = false,
                     has_routine = false, has_id = false;
                reader.object([&](const std::string& field) {
                    if (field == "symbol") { symbol = reader.word(); has_symbol = true; }
                    else if (field == "message") { message = reader.word(); has_message = true; }
                    else if (field == "routine_id") { routine = reader.word(); has_routine = true; }
                    else if (field == "id") { id = reader.word(); has_id = true; }
                    else throw std::runtime_error("unexpected source-log manifest field");
                });
                if (!has_symbol || !has_message || !has_routine || !has_id ||
                    !valid_message(message))
                    throw std::runtime_error("incomplete or unbounded static log site");
                const auto [name, arity] = selector(symbol);
                telemetry::SourceId parsed{}, parent{};
                if (!telemetry::parse_source_id(routine,
                        telemetry::SourceKind::Routine, parent) ||
                    !telemetry::parse_source_id(id,
                        telemetry::SourceKind::Site, parsed))
                    throw std::runtime_error("invalid routine or site ID in static log manifest");
                manifest.sites.push_back({name, arity, message, routine, id});
            });
        } else {
            throw std::runtime_error("unrecognized native source-log manifest field");
        }
    });
    reader.done();
    if (!schema || !unit || !routines || !sites ||
        manifest.source_unit != expected_source_unit)
        throw std::runtime_error("source-log manifest lacks exact source-unit/schema binding");
    std::unordered_map<std::string, std::string> by_function;
    std::unordered_set<std::string> routine_ids, site_ids, site_keys;
    for (const auto& entry : manifest.routines) {
        std::string key = entry.symbol + "/" + std::to_string(entry.arity);
        if (!by_function.emplace(key, entry.routine_id).second ||
            !routine_ids.insert(entry.routine_id).second)
            throw std::runtime_error("duplicate routine selector/identity in source manifest");
    }
    for (const auto& entry : manifest.sites) {
        std::string key = entry.symbol + "/" + std::to_string(entry.arity);
        const auto owner = by_function.find(key);
        if (owner == by_function.end() || owner->second != entry.routine_id ||
            !site_ids.insert(entry.site_id).second ||
            !site_keys.insert(key + "\n" + entry.message).second)
            throw std::runtime_error("unowned or duplicate native static log site");
    }
    if (manifest.routines.empty())
        throw std::runtime_error("source-log manifest contains no callable coverage");
    return manifest;
}

} // namespace oreslang
