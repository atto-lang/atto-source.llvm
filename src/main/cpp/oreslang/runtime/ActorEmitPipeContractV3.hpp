#pragma once
#include "oreslang/runtime/ActorPipeContract.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * AttoLang v3 compiler-only actor-pipe semantic admission.
 *
 * This contract models the *actual types of emit(expr)* found by a typed
 * receive-body checker, never an inference that input type equals output.
 * It does not construct an ActorRef, transfer a heap, allocate a mailbox or
 * admit native guest execution. The native frontend must lower and authorize
 * source-level send, emit and supervisor-bound links before claiming parity.
 *
 * Legacy explicit PipeRole/DAG API remains independent and unchanged.
 */
struct ActorEmitStageV3 {
    std::string instance_id;
    std::string actor_type;
    std::string receive_type;
    std::vector<std::string> emit_types;
    bool supervisor = false;
};

inline void require_actor_emit_scalar_v3(const std::string& type) {
    if (type != "int" && type != "bool")
        throw std::invalid_argument(
            "E_ACTOR_PIPE_PAYLOAD_DOMAIN: v3 ActorMail/emit requires nominal int or bool");
}

inline std::vector<PipeConnection> check_actor_emit_pipeline_v3(
        const std::vector<ActorEmitStageV3>& stages) {
    constexpr std::size_t max_stages = 16;
    constexpr std::size_t max_emit_observations = 1024;
    if (stages.size() < 2 || stages.size() > max_stages)
        throw std::invalid_argument(
            "E_ACTOR_PIPE_LIMIT: pipeline requires 2..16 distinct actor instances");

    std::unordered_set<std::string> seen;
    std::vector<PipeConnection> links;
    links.reserve(stages.size() - 1);
    std::string previous_output;
    std::string previous_id;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& stage = stages[i];
        if (!valid_pipe_component(stage.instance_id)
                || !valid_pipe_component(stage.actor_type))
            throw std::invalid_argument(
                "E_ACTOR_PIPE_IDENTITY: actor type and instance IDs must be strict bounded UTF-8");
        require_actor_emit_scalar_v3(stage.receive_type);
        if (stage.emit_types.size() > max_emit_observations)
            throw std::invalid_argument(
                "E_ACTOR_PIPE_EMIT_LIMIT: actor body exceeds 1024 typed emit observations");
        if (!seen.insert(stage.instance_id).second)
            throw std::invalid_argument(
                "E_ACTOR_PIPE_DUPLICATE_INSTANCE: actor instance was attached more than once");
        if (stage.supervisor)
            throw std::invalid_argument(
                "E_ACTOR_PIPE_SUPERVISOR: CONTROL supervisor cannot be a DATA stage");

        std::string output;
        for (const auto& ty : stage.emit_types) {
            require_actor_emit_scalar_v3(ty);
            if (!output.empty() && output != ty)
                throw std::invalid_argument(
                    "E_ACTOR_PIPE_EMIT_TYPE: actor emits incompatible scalar types");
            output = ty;
        }
        if (i > 0) {
            if (previous_output != stage.receive_type)
                throw std::invalid_argument(
                    "E_ACTOR_PIPE_EMIT_TYPE: previous stage emitted " + previous_output
                    + " but " + stage.instance_id + " receives " + stage.receive_type);
            links.push_back({previous_id, stage.instance_id, previous_output});
        }
        if (i + 1 == stages.size()) {
            if (!output.empty())
                throw std::invalid_argument(
                    "E_ACTOR_PIPE_SINK_EMIT: terminal sink cannot emit without downstream");
        } else if (output.empty()) {
            throw std::invalid_argument(
                "E_ACTOR_PIPE_STAGE_NO_EMIT: nonterminal stage must emit a typed value");
        }
        previous_output = std::move(output);
        previous_id = stage.instance_id;
    }
    return links;
}

} // namespace oreslang::runtime
