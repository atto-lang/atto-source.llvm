#pragma once

#include "oreslang/frontend/QuantityUnit.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace oreslang {

struct ProtocolScalar {
    bool i32;
    std::int64_t minimum;
    std::int64_t maximum;
    QuantityUnit unit;

    bool operator==(const ProtocolScalar& other) const {
        return i32 == other.i32 && minimum == other.minimum && maximum == other.maximum && unit == other.unit;
    }
};

struct ProtocolField {
    std::string name;
    ProtocolScalar type;
};

struct ProtocolEdge {
    std::string source, target, message;
    std::vector<ProtocolField> payload;
};

// Source-level graph only. It grants no actor or mailbox authority.
struct VerifiedProtocol {
    std::string name;
    std::string initial_state;
    std::unordered_set<std::string> terminal_states;
    std::vector<ProtocolEdge> edges;
};

} // namespace oreslang
