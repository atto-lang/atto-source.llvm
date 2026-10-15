#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * CPU host-only deterministic batched Mixture-of-Experts planner.
 * Does not allocate tensors, execute experts, mint GPU leases or lower source.
 * Mirrors the Java NeuralMoERoutingPlan admission/normalization contract.
 */
class NeuralMoERoutingPlan final {
public:
    static constexpr std::size_t MAX_TOKENS = 4096;
    static constexpr std::size_t MAX_EXPERTS = 256;
    static constexpr std::size_t MAX_TOP_K = 8;

    enum class OverflowPolicy { reject, drop_token };

    struct ExpertScore {
        std::size_t expert_index;
        double score;
    };

    struct TokenScores {
        std::uint64_t token_index;
        std::vector<ExpertScore> scores;
    };

    struct Assignment {
        std::uint64_t token_index;
        std::size_t expert_index;
        double normalized_weight;
    };

    struct Plan {
        std::vector<Assignment> assignments;
        std::vector<std::uint64_t> dropped_token_indices;
        std::vector<std::size_t> expert_loads;
        std::size_t top_k;
        std::size_t per_expert_capacity;
    };

    static Plan plan(
            const std::vector<TokenScores>& tokens,
            std::size_t expert_count,
            std::size_t top_k,
            std::size_t per_expert_capacity,
            OverflowPolicy policy) {
        if (tokens.empty() || tokens.size() > MAX_TOKENS)
            throw std::invalid_argument("MoE token batch requires 1..4096 items");
        if (expert_count == 0 || expert_count > MAX_EXPERTS
                || top_k == 0 || top_k > MAX_TOP_K || top_k > expert_count)
            throw std::invalid_argument("invalid MoE expert or top-k configuration");
        if (per_expert_capacity == 0 || per_expert_capacity > MAX_TOKENS)
            throw std::invalid_argument("invalid per-expert token capacity");
        if (policy != OverflowPolicy::reject && policy != OverflowPolicy::drop_token)
            throw std::invalid_argument("invalid MoE overflow policy");

        Plan result{{}, {}, std::vector<std::size_t>(expert_count, 0),
                    top_k, per_expert_capacity};
        std::set<std::uint64_t> seen_tokens;

        for (const auto& token : tokens) {
            if (!seen_tokens.insert(token.token_index).second)
                throw std::invalid_argument("duplicate MoE token index");
            if (token.scores.size() != expert_count)
                throw std::invalid_argument("every token requires one score per expert");

            std::vector<bool> expert_seen(expert_count, false);
            double maximum = 0.0;
            for (const auto& score : token.scores) {
                if (score.expert_index >= expert_count || expert_seen[score.expert_index]
                        || !std::isfinite(score.score) || score.score < 0.0)
                    throw std::invalid_argument("invalid or duplicated expert score");
                expert_seen[score.expert_index] = true;
                maximum = std::max(maximum, score.score);
            }
            if (maximum == 0.0)
                throw std::invalid_argument("all-zero expert scores cannot define routing weights");

            std::vector<ExpertScore> sorted = token.scores;
            std::sort(sorted.begin(), sorted.end(),
                [](const ExpertScore& left, const ExpertScore& right) {
                    if (left.score != right.score) return left.score > right.score;
                    return left.expert_index < right.expert_index;
                });

            // Capacity is checked for every selected expert before committing
            // any load; no token can be partially routed to top-k.
            bool capacity_available = true;
            for (std::size_t i = 0; i < top_k; ++i) {
                if (result.expert_loads[sorted[i].expert_index] >= per_expert_capacity) {
                    capacity_available = false;
                    break;
                }
            }
            if (!capacity_available) {
                if (policy == OverflowPolicy::reject)
                    throw std::overflow_error("MoE expert routing capacity exceeded");
                result.dropped_token_indices.push_back(token.token_index);
                continue;
            }
            // Scale before summing so very large finite scores cannot overflow.
            double scaled_sum = 0.0;
            for (std::size_t i = 0; i < top_k; ++i)
                scaled_sum += sorted[i].score / maximum;
            if (!std::isfinite(scaled_sum) || scaled_sum <= 0.0)
                throw std::invalid_argument("invalid MoE top-k weight normalization");

            for (std::size_t i = 0; i < top_k; ++i) {
                const auto& expert = sorted[i];
                ++result.expert_loads[expert.expert_index];
                result.assignments.push_back(Assignment{
                    token.token_index, expert.expert_index,
                    (expert.score / maximum) / scaled_sum
                });
            }
        }
        return result;
    }
};

} // namespace oreslang::runtime
