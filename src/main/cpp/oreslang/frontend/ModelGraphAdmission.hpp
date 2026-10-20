#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oreslang::model {

// Typed HIR admission prototype; never guest-executable. The signatures must
// originate from a compiler-owned registry, not a source or wire payload.
// V1 admits ordered, pure, single-input/single-output nominal operations.
struct Port {
    std::string dtype;
    std::vector<std::int64_t> shape;
    std::string unit;

    bool operator==(const Port& other) const {
        return dtype == other.dtype && shape == other.shape && unit == other.unit;
    }
};
struct Signature {
    std::string name;
    std::optional<Port> input;
    Port output;
    std::string effect;
    std::string abi;
    std::string impl_digest;
    std::string provenance;
    std::int64_t scratch_bytes = 0;
    bool vjp = false;
};
struct Limits {
    std::size_t max_nodes = 64;
    std::size_t max_depth = 64;
    std::int64_t max_bytes = 65536;
};
struct Node {
    std::size_t id = 0;
    std::string op;
    std::string abi;
    std::string impl_digest;
    std::string provenance;
    std::vector<std::string> config;
    std::optional<Port> input;
    Port output;
    bool vjp = false;
};
struct ModelGraph {
    std::string name;
    std::optional<Port> input;
    Port output;
    std::vector<std::string> formal_config;
    std::vector<Node> nodes;
    std::int64_t upper_bound_bytes = 0;
};
using Registry = std::unordered_map<std::string, Signature>;

inline void require(bool valid, const char* diagnostic) {
    if (!valid) throw std::runtime_error(diagnostic);
}
inline bool digest_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
inline bool valid_digest(const std::string& digest) {
    if (digest.size() != 64) return false;
    for (char c : digest) if (!digest_hex(c)) return false;
    return true;
}
inline bool unit_character(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '_' ||
           c == '^' || c == '-';
}
inline std::int64_t port_bytes(const Port& port) {
    std::int64_t item = 0;
    if (port.dtype == "bool") item = 1;
    else if (port.dtype == "i32" || port.dtype == "f32") item = 4;
    else if (port.dtype == "i64" || port.dtype == "f64") item = 8;
    else throw std::runtime_error("MODEL_INVALID_PORT");
    require(port.shape.size() <= 8 && port.unit.size() <= 40, "MODEL_INVALID_PORT");
    for (char c : port.unit) require(unit_character(c), "MODEL_INVALID_PORT");
    bool empty = false;
    for (std::int64_t d : port.shape) {
        require(d >= 0, "MODEL_INVALID_EXTENT");
        if (d == 0) empty = true;
    }
    if (empty) return 0;
    for (std::int64_t d : port.shape) {
        if (d > 0 && item > std::numeric_limits<std::int64_t>::max() / d)
            throw std::runtime_error("MODEL_SIZE_OVERFLOW");
        item *= d;
    }
    return item;
}
inline void charge(std::int64_t& total, std::int64_t amount,
                   std::int64_t limit) {
    require(amount >= 0, "MODEL_INVALID_SCRATCH");
    if (amount > std::numeric_limits<std::int64_t>::max() - total)
        throw std::runtime_error("MODEL_SIZE_OVERFLOW");
    total += amount;
    if (total > limit) throw std::runtime_error("MODEL_MEMORY_BUDGET");
}
inline void validate_default(const ModelFormal& formal) {
    if (formal.default_value.empty()) return;
    const bool has_point = formal.default_value.find('.') != std::string::npos;
    bool valid = false;
    if (formal.type == "int") valid = !has_point;
    else if (formal.type == "float" || formal.type == "f32" || formal.type == "f64")
        valid = has_point;
    else if (formal.type == "bool")
        valid = formal.default_value == "true" || formal.default_value == "false";
    require(valid, "MODEL_FORMAL_DEFAULT_TYPE_MISMATCH");
}

inline ModelGraph admit(const ModelDecl& source, const Registry& trusted,
                        Limits limits = {}) {
    require(!source.operations.empty(), "MODEL_EMPTY_BODY");
    require(limits.max_nodes > 0 && limits.max_depth > 0 && limits.max_bytes >= 0,
            "MODEL_INVALID_BUDGET");
    require(source.operations.size() <= limits.max_nodes, "MODEL_NODE_BUDGET");
    require(source.operations.size() <= limits.max_depth, "MODEL_DEPTH_BUDGET");
    require(source.formals.size() <= 16, "MODEL_FORMAL_BUDGET");
    std::unordered_set<std::string> formals;
    ModelGraph result;
    for (const auto& formal : source.formals) {
        require(formals.insert(formal.name).second, "MODEL_DUPLICATE_FORMAL");
        validate_default(formal);
        const auto def = formal.default_value.empty() ? std::string("none") : formal.default_value;
        result.formal_config.push_back(std::to_string(formal.name.size()) + ":" + formal.name + "|"
            + std::to_string(formal.type.size()) + ":" + formal.type + "|" + def);
    }
    result.name = source.name;
    std::optional<Port> prior;
    for (const auto& operation : source.operations) {
        const auto it = trusted.find(operation.name);
        if (it == trusted.end() || it->second.name != operation.name)
            throw std::runtime_error("MODEL_UNKNOWN_OP");
        const auto& sig = it->second;
        require(sig.effect == "pure", "MODEL_EFFECTFUL_OP");
        require(!sig.abi.empty() && sig.abi.size() <= 64, "MODEL_INVALID_OP_ABI");
        for (char c : sig.abi)
            require(unit_character(c) || c == '.', "MODEL_INVALID_OP_ABI");
        require(valid_digest(sig.impl_digest), "MODEL_INVALID_IMPL_DIGEST");
        require(sig.provenance == "verified-ores" || sig.provenance == "trusted-native",
                "MODEL_INVALID_OP_PROVENANCE");
        require(sig.scratch_bytes >= 0, "MODEL_INVALID_SCRATCH");
        std::vector<std::string> config;
        for (const auto& argument : operation.arguments) {
            if (argument.formal_reference)
                require(formals.find(argument.value) != formals.end(),
                        "MODEL_UNKNOWN_FORMAL");
            config.push_back(std::to_string(argument.label.size()) + ":" + argument.label + "|"
                + (argument.formal_reference ? "FORMAL|" : "LITERAL|")
                + std::to_string(argument.value.size()) + ":" + argument.value);
        }
        const auto output_bytes = port_bytes(sig.output);
        if (sig.input) (void)port_bytes(*sig.input);
        if (result.nodes.empty()) {
            result.input = sig.input;
            if (result.input)
                charge(result.upper_bound_bytes, port_bytes(*result.input), limits.max_bytes);
        } else {
            require(prior == sig.input, "MODEL_PORT_TYPE_MISMATCH");
        }
        charge(result.upper_bound_bytes, output_bytes, limits.max_bytes);
        charge(result.upper_bound_bytes, sig.scratch_bytes, limits.max_bytes);
        result.nodes.push_back({result.nodes.size(), operation.name, sig.abi,
                                sig.impl_digest, sig.provenance, config,
                                sig.input, sig.output, sig.vjp});
        prior = sig.output;
    }
    result.output = *prior;
    return result;
}

} // namespace oreslang::model
