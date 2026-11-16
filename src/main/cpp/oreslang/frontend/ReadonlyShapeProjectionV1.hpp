#pragma once
// Compiler-only, bounded, non-emitting readonly i64 shape projection proof.
// This does NOT perform native guest codegen, reinterpret class pointers,
// authorize mutation, extend loans across await or open actor/map state.
// Source descriptors must be constructed from verified type-checker metadata.
#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::frontend::shape_v1 {
constexpr std::size_t max_fields = 32;
enum class origin { class_, record, struct_, map, actor };
enum class scalar { i64, boolean, string };

struct field {
    std::string name;
    scalar type;
    std::int32_t slot;
    bool is_public;
    bool immutable;
    bool effect_free;
    bool sealed_actor_state;
};
struct required_field { std::string name; scalar type; };
struct source {
    origin kind;
    std::uint32_t nominal_id;
    std::uint64_t layout_epoch;
    std::uint64_t loan_id;
    bool sealed_actor_state;
    std::vector<field> fields;
};
struct field_witness {
    std::string name;
    scalar type;
    std::int32_t source_slot;
};
struct plan {
    std::uint32_t source_nominal_id;
    std::uint64_t layout_epoch;
    std::uint64_t loan_id;
    std::vector<field_witness> fields;

    const field_witness& get_field(const std::string& name) const {
        auto found = std::find_if(fields.begin(), fields.end(),
            [&](const field_witness& f) { return f.name == name; });
        if (found == fields.end()) {
            throw std::invalid_argument("field not in projected readonly row");
        }
        return *found;
    }
};

inline plan prove(const source& actual, const std::vector<required_field>& requested) {
    if (actual.kind != origin::class_ && actual.kind != origin::record &&
        actual.kind != origin::struct_) {
        throw std::invalid_argument("dynamic maps and actor states require another ABI");
    }
    if (actual.sealed_actor_state) {
        throw std::invalid_argument("sealed actor state cannot be structurally projected");
    }
    if (!actual.nominal_id || !actual.layout_epoch || !actual.loan_id) {
        throw std::invalid_argument("unproven source identity, layout or readonly loan");
    }
    if (requested.empty() || requested.size() > max_fields ||
        actual.fields.size() > max_fields) {
        throw std::invalid_argument("empty or oversized structural row");
    }
    std::set<std::string> source_names;
    std::set<std::int32_t> source_slots;
    for (const auto& f : actual.fields) {
        if (f.name.empty() || !source_names.insert(f.name).second) {
            throw std::invalid_argument("duplicate source field");
        }
        if (f.slot < 0 || !source_slots.insert(f.slot).second) {
            throw std::invalid_argument("invalid or overlapping source offset");
        }
    }
    std::set<std::string> required_names;
    std::vector<field_witness> verified;
    verified.reserve(requested.size());
    for (const auto& wanted : requested) {
        if (wanted.name.empty() || !required_names.insert(wanted.name).second) {
            throw std::invalid_argument("duplicate required field");
        }
        auto found = std::find_if(actual.fields.begin(), actual.fields.end(),
            [&](const field& f) { return f.name == wanted.name; });
        if (found == actual.fields.end()) {
            throw std::invalid_argument("missing source field in readonly row");
        }
        if (found->sealed_actor_state) {
            throw std::invalid_argument("protected actor field");
        }
        if (wanted.type != scalar::i64 || found->type != scalar::i64 ||
            !found->is_public || !found->immutable || !found->effect_free ||
            found->type != wanted.type) {
            throw std::invalid_argument("public immutable field type mismatch");
        }
        verified.push_back({found->name, found->type, found->slot});
    }
    std::sort(verified.begin(), verified.end(),
        [](const field_witness& a, const field_witness& b) {
            return a.name < b.name;
        });
    return {actual.nominal_id, actual.layout_epoch, actual.loan_id,
            std::move(verified)};
}
} // namespace oreslang::frontend::shape_v1
