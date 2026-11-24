#pragma once

#include "oreslang/frontend/QuantityUnit.hpp"

#include <cstdint>
#include <memory>
#include <optional>
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

struct ProtocolHandler {
    std::string message;
    std::vector<ProtocolField> parameters;
    std::string destination;
    std::optional<std::uint16_t> increment_parameter;
};

struct ProtocolStateField {
    ProtocolField field;
    std::int64_t initial;
};

struct VerifiedProtocolActor {
    std::string name;
    std::shared_ptr<const VerifiedProtocol> protocol;
    std::vector<ProtocolHandler> handlers;
    std::optional<ProtocolStateField> field;
};

struct ProtocolGuestTurn {
    std::string message;
    std::vector<std::int64_t> payload;
};

struct VerifiedProtocolGuest {
    std::string actor;
    std::vector<ProtocolGuestTurn> turns;
    std::size_t result;
};

} // namespace oreslang
