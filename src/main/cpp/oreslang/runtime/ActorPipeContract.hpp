#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/** Trusted graph strings have the same bounded strict UTF-8 representation as
 * the Java admission plan. A graph identifier must never contain NUL, invalid
 * UTF-8, surrogates, or a truncation ambiguity across the ABI boundary. */
inline bool valid_pipe_component(const std::string& value,
                                 bool optional = false) noexcept {
    if (value.empty()) return optional;
    if (value.size() > 256) return false;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        if (lead == 0) return false;
        if (lead <= 0x7f) { ++i; continue; }
        std::size_t width = 0;
        if (lead >= 0xc2 && lead <= 0xdf) width = 2;
        else if (lead >= 0xe0 && lead <= 0xef) width = 3;
        else if (lead >= 0xf0 && lead <= 0xf4) width = 4;
        else return false;
        if (width > value.size() - i) return false;
        for (std::size_t n = 1; n < width; ++n) {
            const auto next = static_cast<unsigned char>(value[i + n]);
            if ((next & 0xc0) != 0x80) return false;
        }
        const auto second = static_cast<unsigned char>(value[i + 1]);
        if ((lead == 0xe0 && second < 0xa0)
            || (lead == 0xed && second > 0x9f)
            || (lead == 0xf0 && second < 0x90)
            || (lead == 0xf4 && second > 0x8f)) return false;
        i += width;
    }
    return true;
}

/** Shared, byte-stable v1 routing key validation. Unlike a graph name,
 * an affinity key may include literal 0x00, but must be well-formed UTF-8,
 * at most 4096 bytes and never hash a truncated ABI representation. */
inline bool valid_pipe_route_key(const std::string& value) noexcept {
    if (value.size() > 4096) return false;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        if (lead <= 0x7f) { ++i; continue; }
        std::size_t width = 0;
        if (lead >= 0xc2 && lead <= 0xdf) width = 2;
        else if (lead >= 0xe0 && lead <= 0xef) width = 3;
        else if (lead >= 0xf0 && lead <= 0xf4) width = 4;
        else return false;
        if (width > value.size() - i) return false;
        for (std::size_t j = 1; j < width; ++j) {
            const auto b = static_cast<unsigned char>(value[i + j]);
            if ((b & 0xc0) != 0x80) return false;
        }
        const auto second = static_cast<unsigned char>(value[i + 1]);
        if ((lead == 0xe0 && second < 0xa0)
            || (lead == 0xed && second > 0x9f)
            || (lead == 0xf0 && second < 0x90)
            || (lead == 0xf4 && second > 0x8f)) return false;
        i += width;
    }
    return true;
}

/** Unsigned FNV-1a64, modulo the EXACT ordered destination count.
 * Selection affinity is not a security capability or actor authorization. */
inline std::size_t actor_pipe_key_slot(const std::string& key,
                                      std::size_t destinations) {
    if (destinations == 0 || destinations > 4096)
        throw std::invalid_argument("pipeline keyed route destination count invalid");
    if (!valid_pipe_route_key(key))
        throw std::invalid_argument("pipeline route key must be bounded UTF-8");
    std::uint64_t digest = UINT64_C(14695981039346656037);
    for (const unsigned char byte : key) {
        digest ^= byte;
        digest *= UINT64_C(1099511628211);
    }
    return static_cast<std::size_t>(digest % destinations);
}

/** Nominal actor-pipeline admission; not an executable transport. */
enum class PipeRole {
    readable, transform, writable, duplex, ordinary_actor, supervisor_actor
};

struct PipeStage {
    std::string actor_type;
    PipeRole role;
    std::string inbound;
    std::string outbound;
    // Optional supervisor-assigned instance identity for a graph. This is
    // NOT a capability. Legacy 4-field stages use actor_type as their ID.
    // Multiple instances can share one nominal actor_type but not an ID.
    std::string instance_id{};
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
    if (stages.size() > 1024)
        throw std::invalid_argument("actor pipeline exceeds stage budget");
    std::unordered_set<std::string> instance_ids;
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
    // Do not mask established role/port diagnostics (e.g. an illegal
    // source or middle stage) with a duplicate-ID error. Validate instance
    // uniqueness only after all existing stage contracts have passed.
    for (const auto& stage : stages) {
        const auto& actor_id =
                stage.instance_id.empty() ? stage.actor_type : stage.instance_id;
        if (!valid_pipe_component(stage.actor_type)
                || !valid_pipe_component(actor_id)
                || !valid_pipe_component(stage.inbound, true)
                || !valid_pipe_component(stage.outbound, true)
                || !instance_ids.insert(actor_id).second)
            throw std::invalid_argument(
                    "linear pipeline requires unique bounded actor instance IDs");
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
        connections.push_back({
                source.instance_id.empty() ? source.actor_type : source.instance_id,
                target.instance_id.empty() ? target.actor_type : target.instance_id,
                source.outbound});
    }
    return connections;
}


/**
 * Checked, acyclic, multi-consumer graph. The runtime still owns edge
 * admission, backpressure and SourceTask suspension; this validates only
 * host graph topology and nominal payload contracts, not guest lowering.
 */
inline std::vector<PipeConnection> check_actor_pipeline_graph(
        const std::vector<PipeStage>& stages,
        const std::vector<PipeConnection>& links) {
    if (stages.size() < 2 || stages.size() > 1024 ||
            links.empty() || links.size() > 4096)
        throw std::invalid_argument("pipeline graph stage/edge budget invalid");
    std::unordered_map<std::string, std::size_t> ids;
    ids.reserve(stages.size());
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& s = stages[i];
        const auto& actor_id = s.instance_id.empty() ? s.actor_type : s.instance_id;
        if (!valid_pipe_component(s.actor_type)
                || !valid_pipe_component(actor_id)
                || !valid_pipe_component(s.inbound, true)
                || !valid_pipe_component(s.outbound, true)
                || !ids.emplace(actor_id, i).second)
            throw std::invalid_argument(
                "pipeline graph requires distinct, bounded actor instance IDs");
        switch (s.role) {
        case PipeRole::readable:
            if (!s.inbound.empty() || s.outbound.empty())
                throw std::invalid_argument("readable pipeline stage must only emit");
            break;
        case PipeRole::writable:
            if (s.inbound.empty() || !s.outbound.empty())
                throw std::invalid_argument("writable pipeline stage must only receive");
            break;
        case PipeRole::transform:
        case PipeRole::duplex:
            if (s.inbound.empty() || s.outbound.empty())
                throw std::invalid_argument("pipeline transform requires input and output");
            break;
        default:
            throw std::invalid_argument("pipeline graph requires an explicit pipe actor role");
        }
    }

    std::unordered_set<std::string> pairs;
    std::vector<std::vector<std::size_t>> outgoing(stages.size());
    std::vector<std::size_t> indegree(stages.size(), 0);
    std::vector<std::size_t> incoming_count(stages.size(), 0);
    std::vector<std::size_t> outgoing_count(stages.size(), 0);
    for (const auto& link : links) {
        if (!valid_pipe_component(link.source)
                || !valid_pipe_component(link.target)
                || !valid_pipe_component(link.payload))
            throw std::invalid_argument("pipeline link has invalid UTF-8 component");
        const auto source = ids.find(link.source);
        const auto target = ids.find(link.target);
        if (source == ids.end() || target == ids.end() ||
                source->second == target->second)
            throw std::invalid_argument("pipeline graph link has unknown or self endpoint");
        const auto& from = stages[source->second];
        const auto& to = stages[target->second];
        if (from.role == PipeRole::writable || to.role == PipeRole::readable ||
                link.payload.empty() || link.payload != from.outbound ||
                link.payload != to.inbound)
            throw std::invalid_argument("pipeline graph link violates typed direction");
        // Use the internal numeric IDs, so even a delimiter inside an actor
        // name cannot cause a false duplicate or an edge-key collision.
        const std::string pair = std::to_string(source->second) + ":" +
                                 std::to_string(target->second);
        if (!pairs.insert(pair).second)
            throw std::invalid_argument("pipeline graph contains a duplicate edge");
        outgoing[source->second].push_back(target->second);
        ++outgoing_count[source->second];
        ++incoming_count[target->second];
        ++indegree[target->second];
    }

    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& stage = stages[i];
        if (incoming_count[i] == 0 && outgoing_count[i] == 0)
            throw std::invalid_argument("pipeline graph contains an isolated actor");
        if ((stage.role == PipeRole::readable && outgoing_count[i] == 0) ||
            (stage.role == PipeRole::writable && incoming_count[i] == 0) ||
            (stage.role == PipeRole::transform &&
              (incoming_count[i] == 0 || outgoing_count[i] == 0)))
            throw std::invalid_argument("pipeline graph lacks a required actor port");
    }
    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i)
        if (indegree[i] == 0) ready.push_back(i);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto target : outgoing[current])
            if (--indegree[target] == 0) ready.push_back(target);
    }
    if (visited != stages.size())
        throw std::invalid_argument("pipeline graph contains a cycle");
    return links;
}

} // namespace oreslang::runtime
