#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ModelGraphAdmission.hpp"
#include "oreslang/frontend/PortableModelGraphDigestV1.hpp"
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::model::resolved_v1 {

using portable_v1::Constant;
using portable_v1::Graph;
using portable_v1::Node;
using portable_v1::Signature;
using Registry = std::unordered_map<std::string, Signature>;
using Overrides = std::unordered_map<std::string, std::string>;

inline Constant canonical_value(const std::string& value, const std::string& kind) {
    if (kind == "i64") {
        if (!portable_v1::canonical_i64_syntax(value))
            throw std::runtime_error("MODEL_CONST_TYPE_MISMATCH");
        std::int64_t parsed = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec == std::errc::result_out_of_range)
            throw std::runtime_error("MODEL_CONST_OVERFLOW");
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            throw std::runtime_error("MODEL_INVALID_CONST");
        return {"i64", std::to_string(parsed)};
    }
    if (kind == "bool") {
        if (value != "true" && value != "false")
            throw std::runtime_error("MODEL_CONST_TYPE_MISMATCH");
        return {"bool", value};
    }
    throw std::runtime_error("MODEL_NONCONSTANT_CONFIG");
}

inline Graph lower(const ModelDecl& source,
                   const std::string& model_type_id,
                   const Registry& trusted,
                   const Overrides& overrides = {}) {
    if (source.operations.empty()) throw std::runtime_error("MODEL_EMPTY_BODY");
    if (source.operations.size() > 64) throw std::runtime_error("MODEL_NODE_BUDGET");
    if (source.formals.size() > 16) throw std::runtime_error("MODEL_FORMAL_BUDGET");
    std::unordered_map<std::string,std::string> config;
    std::unordered_set<std::string> declared;
    for (const auto& formal : source.formals) {
        if (!model_identifier(formal.name))
            throw std::runtime_error("MODEL_NONCONSTANT_CONFIG");
        if (!declared.insert(formal.name).second)
            throw std::runtime_error("MODEL_DUPLICATE_FORMAL");
        // Validate even unused or overridden defaults before dead fields
        // can be silently omitted from the emitted portable graph.
        validate_default(formal);
        auto override = overrides.find(formal.name);
        if (override != overrides.end()) {
            validate_scalar(override->second, false, false, formal.type);
            config.emplace(formal.name, override->second);
        } else if (!formal.default_value.empty())
            config.emplace(formal.name, formal.default_value);
        else throw std::runtime_error("MODEL_NONCONSTANT_CONFIG");
    }
    for (const auto& [name, value] : overrides) {
        (void)value;
        if (!declared.count(name)) throw std::runtime_error("MODEL_UNKNOWN_FORMAL");
    }

    Graph graph;
    graph.version = 1;
    graph.model_type_id = model_type_id;
    std::string prior;
    for (std::size_t index = 0; index < source.operations.size(); ++index) {
        const auto& op = source.operations[index];
        auto found = trusted.find(op.name);
        if (found == trusted.end()) throw std::runtime_error("MODEL_UNKNOWN_OP");
        const auto& sig = found->second;
        if (sig.effect != "pure") throw std::runtime_error("MODEL_EFFECTFUL_OP");
        if (index == 0 && sig.inputs.size() > 1) throw std::runtime_error("MODEL_ARITY_MISMATCH");
        if (index > 0 && sig.inputs.size() != 1) throw std::runtime_error("MODEL_ARITY_MISMATCH");
        if (op.arguments.size() > 16) throw std::runtime_error("MODEL_ARGUMENT_BUDGET");
        if (op.arguments.size() != sig.const_kinds.size())
            throw std::runtime_error("MODEL_CONST_ARITY_MISMATCH");

        std::vector<Constant> constants;
        for (std::size_t i = 0; i < op.arguments.size(); ++i) {
            // V1 has no linker-verified config names. Erasing a named label
            // would alias semantically distinct source calls under one digest.
            if (!op.arguments[i].label.empty())
                throw std::runtime_error("MODEL_NAMED_ARGUMENT_UNRESOLVED");
            std::string value = op.arguments[i].value;
            if (op.arguments[i].formal_reference) {
                auto it = config.find(value);
                if (it == config.end()) throw std::runtime_error("MODEL_UNKNOWN_FORMAL");
                value = it->second;
            }
            validate_scalar(value, false, false,
                            sig.const_kinds[i] == "i64" ? "int" : sig.const_kinds[i]);
            constants.push_back(canonical_value(value, sig.const_kinds[i]));
        }

        Node node;
        node.id = "n" + std::to_string(index);
        node.op = op.name;
        if (!prior.empty()) node.inputs.push_back(prior);
        node.const_args = std::move(constants);
        node.signature = sig;
        prior = node.id;
        graph.nodes.push_back(std::move(node));
    }
    graph.outputs.push_back(prior);
    // Fail closed at the lowering boundary, not only if the caller later
    // chooses to request a portable hash.
    (void)portable_v1::canonical_bytes(graph);
    return graph;
}

} // namespace oreslang::model::resolved_v1
