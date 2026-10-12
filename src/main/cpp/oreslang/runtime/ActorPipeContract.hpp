#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/** Nominal actor-pipeline admission; not an executable transport. */
enum class PipeRole {
    readable, transform, writable, duplex, ordinary_actor, supervisor_actor
};

struct PipeStage {
    std::string actor_type;
    PipeRole role;
    std::string inbound;
    std::string outbound;
};

struct PipeConnection {
    std::string source;
    std::string target;
    std::string payload;
};

inline std::vector<PipeConnection> check_actor_pipeline(
        const std::vector<PipeStage>& stages) {
    if (stages.size() < 2)
        throw std::invalid_argument("actor pipeline requires at least two stages");
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& stage = stages[i];
        if (stage.actor_type.empty())
            throw std::invalid_argument("pipeline actor type cannot be blank");
        if (stage.role == PipeRole::supervisor_actor)
            throw std::invalid_argument("supervisor actors cannot be pipeline stages");
        if (stage.role == PipeRole::ordinary_actor)
            throw std::invalid_argument("ordinary actor lacks explicit pipe role");
        switch (stage.role) {
            case PipeRole::readable:
                if (!stage.inbound.empty() || stage.outbound.empty())
                    throw std::invalid_argument("readable pipe actor must only emit");
                break;
            case PipeRole::transform:
            case PipeRole::duplex:
                if (stage.inbound.empty() || stage.outbound.empty())
                    throw std::invalid_argument("transform/duplex pipe actor requires input and output");
                break;
            case PipeRole::writable:
                if (stage.inbound.empty() || !stage.outbound.empty())
                    throw std::invalid_argument("writable pipe actor must only receive");
                break;
            default:
                throw std::invalid_argument("invalid pipe actor role");
        }
        if (i == 0 && stage.role != PipeRole::readable
                && stage.role != PipeRole::duplex)
            throw std::invalid_argument("actor pipeline source must be readable or duplex");
        if (i > 0 && i + 1 < stages.size()
                && stage.role != PipeRole::transform
                && stage.role != PipeRole::duplex)
            throw std::invalid_argument("actor pipeline middle stage must transform or duplex");
        if (i + 1 == stages.size() && stage.role != PipeRole::writable
                && stage.role != PipeRole::duplex)
            throw std::invalid_argument("actor pipeline sink must be writable or duplex");
    }
    std::vector<PipeConnection> connections;
    connections.reserve(stages.size() - 1);
    for (std::size_t i = 1; i < stages.size(); ++i) {
        const auto& source = stages[i - 1];
        const auto& target = stages[i];
        if (source.outbound != target.inbound)
            throw std::invalid_argument("actor pipe payload mismatch: "
                    + source.actor_type + " emits " + source.outbound
                    + " but " + target.actor_type + " requires " + target.inbound);
        connections.push_back({source.actor_type, target.actor_type, source.outbound});
    }
    return connections;
}

} // namespace oreslang::runtime
