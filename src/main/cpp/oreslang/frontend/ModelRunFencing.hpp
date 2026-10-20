#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace oreslang::model {

inline bool fence_digest_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
inline void require_fence_id(const std::string& value, const char* diagnostic) {
    if (value.empty() || value.size() > 256) throw std::runtime_error(diagnostic);
}
inline void require_fence_digest(const std::string& value, const char* diagnostic) {
    if (value.size() != 64) throw std::runtime_error(diagnostic);
    for (char c : value) if (!fence_digest_hex(c)) throw std::runtime_error(diagnostic);
}

struct ModelOwner {
    std::string model_type_id;
    std::string graph_digest;
    std::int64_t graph_generation = 0;
    std::string instance_id;
    std::int64_t parameter_version = 0;
    std::string execution_profile_digest;
};

struct RunStamp {
    std::string run_id;
    std::string model_type_id;
    std::string graph_digest;
    std::int64_t graph_generation = 0;
    std::string instance_id;
    std::int64_t parameter_version = 0;
    std::string execution_profile_digest;
};

struct RunLedger {
    std::unordered_set<std::string> committed_run_ids;
};

inline void validate_owner(const ModelOwner& owner) {
    require_fence_id(owner.model_type_id, "MODEL_INVALID_MODEL_TYPE_ID");
    require_fence_digest(owner.graph_digest, "MODEL_INVALID_GRAPH_DIGEST");
    if (owner.graph_generation < 0) throw std::runtime_error("MODEL_INVALID_GRAPH_GENERATION");
    require_fence_id(owner.instance_id, "MODEL_INVALID_INSTANCE_ID");
    if (owner.parameter_version < 0) throw std::runtime_error("MODEL_INVALID_PARAMETER_VERSION");
    require_fence_digest(owner.execution_profile_digest, "MODEL_INVALID_EXECUTION_PROFILE");
}

inline void validate_stamp(const RunStamp& stamp) {
    require_fence_id(stamp.run_id, "MODEL_INVALID_RUN_ID");
    require_fence_id(stamp.model_type_id, "MODEL_INVALID_TYPE_ID");
    require_fence_digest(stamp.graph_digest, "MODEL_INVALID_GRAPH_DIGEST");
    if (stamp.graph_generation < 0) throw std::runtime_error("MODEL_INVALID_GRAPH_GENERATION");
    require_fence_id(stamp.instance_id, "MODEL_INVALID_INSTANCE_ID");
    if (stamp.parameter_version < 0) throw std::runtime_error("MODEL_INVALID_PARAMETER_VERSION");
    require_fence_digest(stamp.execution_profile_digest, "MODEL_INVALID_PROFILE_DIGEST");
}

inline std::int64_t next_graph_generation(std::int64_t current) {
    if (current < 0) throw std::runtime_error("MODEL_INVALID_GRAPH_GENERATION");
    if (current == std::numeric_limits<std::int64_t>::max())
        throw std::runtime_error("MODEL_GRAPH_GENERATION_OVERFLOW");
    return current + 1;
}

inline std::int64_t next_parameter_version(std::int64_t current) {
    if (current < 0) throw std::runtime_error("MODEL_INVALID_PARAMETER_VERSION");
    if (current == std::numeric_limits<std::int64_t>::max())
        throw std::runtime_error("MODEL_PARAMETER_VERSION_OVERFLOW");
    return current + 1;
}

inline RunLedger validate_and_commit(const ModelOwner& owner,
                                     const RunStamp& stamp,
                                     const RunLedger& ledger) {
    validate_owner(owner);
    validate_stamp(stamp);
    if (ledger.committed_run_ids.count(stamp.run_id))
        throw std::runtime_error("MODEL_RUN_REPLAY");
    if (owner.model_type_id != stamp.model_type_id)
        throw std::runtime_error("MODEL_INSTANCE_TYPE_MISMATCH");
    if (owner.graph_digest != stamp.graph_digest)
        throw std::runtime_error("MODEL_STALE_GRAPH");
    if (owner.graph_generation != stamp.graph_generation)
        throw std::runtime_error("MODEL_STALE_GRAPH_GENERATION");
    if (owner.instance_id != stamp.instance_id)
        throw std::runtime_error("MODEL_WRONG_INSTANCE");
    if (owner.parameter_version != stamp.parameter_version)
        throw std::runtime_error("MODEL_STALE_PARAMETER_VERSION");
    if (owner.execution_profile_digest != stamp.execution_profile_digest)
        throw std::runtime_error("MODEL_WRONG_EXECUTION_PROFILE");
    RunLedger next = ledger;
    next.committed_run_ids.insert(stamp.run_id);
    return next;
}

} // namespace oreslang::model
