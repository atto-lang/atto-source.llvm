#pragma once
#include <algorithm>
#include <cstddef>
#include <set>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace atto::parametric {
// Staged binder for already-resolved values. No parser or guest admission.
template<class V> struct Formal {
    std::string name;
    std::function<bool(const V&)> accepts;
};
template<class V> using Argument = std::pair<std::string, V>;
enum class Mode { application, refinement };

template<class V>
std::vector<Argument<V>> bind(const std::vector<Formal<V>>& schema,
        const std::vector<Argument<V>>& seed,
        const std::vector<Argument<V>>& updates, Mode mode) {
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (!schema[i].accepts) throw std::invalid_argument("missing formal validator");
        for (std::size_t j = 0; j < i; ++j)
            if (schema[i].name == schema[j].name)
                throw std::invalid_argument("duplicate formal: " + schema[i].name);
    }
    auto checked = [&](const auto& args) {
        std::vector<std::optional<V>> values(schema.size());
        for (const auto& [name, value] : args) {
            std::size_t i = 0;
            while (i < schema.size() && schema[i].name != name) ++i;
            if (i == schema.size()) throw std::invalid_argument("unknown formal: " + name);
            if (values[i]) throw std::invalid_argument("duplicate argument: " + name);
            if (!schema[i].accepts(value))
                throw std::invalid_argument("invalid argument kind or bound: " + name);
            values[i] = value;
        }
        return values;
    };
    auto base = checked(seed);
    auto change = checked(updates);
    std::vector<Argument<V>> result;
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (change[i]) {
            if (base[i] && mode != Mode::refinement)
                throw std::invalid_argument("duplicate argument: " + schema[i].name);
            result.emplace_back(schema[i].name, *change[i]);
        } else if (base[i]) {
            result.emplace_back(schema[i].name, *base[i]);
        }
    }
    return result;
}
// Evidence is already type-checked; this does not perform expression inference.
template<class V>
std::vector<Argument<V>> refine_inferred(const std::vector<Formal<V>>& schema,
        const std::vector<Argument<V>>& seed,
        const std::vector<Argument<V>>& replacements,
        const std::vector<std::string>& holes,
        const std::vector<Argument<V>>& evidence) {
    const auto base = bind(schema, seed, {}, Mode::application);
    const auto explicit_values = bind(schema, replacements, {}, Mode::application);
    const auto observed = bind(schema, evidence, {}, Mode::application);
    auto has = [](const auto& args, const std::string& name) {
        return std::any_of(args.begin(), args.end(), [&](const auto& a) { return a.first == name; });
    };
    std::set<std::string> requested;
    for (const auto& hole : holes) {
        if (!requested.insert(hole).second || !has(base, hole) || has(explicit_values, hole))
            throw std::invalid_argument("invalid inherited inference hole: " + hole);
    }
    if (observed.size() != requested.size())
        throw std::invalid_argument("missing or unexpected inference evidence");
    auto updates = replacements;
    for (const auto& value : observed) {
        if (!requested.count(value.first))
            throw std::invalid_argument("unexpected inference evidence: " + value.first);
        updates.push_back(value);
    }
    return atto::parametric::bind(schema, seed, updates, Mode::refinement);
}
} // namespace atto::parametric
