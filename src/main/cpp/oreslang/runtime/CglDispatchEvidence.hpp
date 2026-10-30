// Offline, allocation-free evaluation of bounded Oreslang actor-dispatch evidence.
// This is a trace validation contract, NOT a realtime scheduler or certification.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ores::cgl {

enum class EvidenceFailure : std::uint8_t {
    none, no_coverage, invalid_config, invalid_identity, invalid_timestamp,
    invalid_cancellation, duplicate_ticket, queue_limit, start_delay, due_to_start, deadline_miss
};

struct DispatchTrace {
    std::uint64_t actor_incarnation;
    std::uint64_t generation;
    std::uint64_t ticket;
    std::uint64_t armed_ns;
    std::uint64_t due_ns;
    std::uint64_t deadline_ns;
    std::uint64_t notified_ns;
    std::uint64_t user_start_ns;
    std::uint64_t user_finish_ns;
    std::uint64_t cancel_ack_ns;
    std::uint32_t queue_depth;
    bool canceled;
};

struct EvidenceLimits {
    std::uint64_t actor_incarnation;
    std::uint64_t generation;
    std::uint64_t max_notify_to_start_ns;
    std::uint64_t max_due_to_start_ns;
    std::uint32_t max_queue_depth;
    std::size_t min_completed;
};

struct EvidenceSummary {
    EvidenceFailure failure = EvidenceFailure::none;
    std::size_t completed = 0;
    std::size_t canceled = 0;
    std::size_t deadline_misses = 0;
    std::size_t start_delay_misses = 0;
    std::size_t due_to_start_misses = 0;
    std::uint64_t max_notify_to_start_ns = 0;
    std::uint64_t max_due_to_start_ns = 0;
    std::uint32_t max_observed_queue_depth = 0;
    // Never treat sample pass as a WCET or schedulability proof.
    bool hard_realtime_qualified = false;
    [[nodiscard]] bool passes_observed_contract() const noexcept {
        return failure == EvidenceFailure::none;
    }
};

// Processes a complete, already captured trace. Never run this on a hard-RT
// dispatch path; duplicate-ticket checking is quadratic in trace count.
[[nodiscard]] inline EvidenceSummary evaluate_dispatch_evidence(
    const DispatchTrace* traces, std::size_t count,
    EvidenceLimits limits) noexcept {
    EvidenceSummary result;
    if (limits.actor_incarnation == 0 || limits.generation == 0 ||
        limits.max_notify_to_start_ns == 0 || limits.max_due_to_start_ns == 0 ||
        limits.max_queue_depth == 0 ||
        limits.min_completed == 0) {
        result.failure = EvidenceFailure::invalid_config;
        return result;
    }
    if (traces == nullptr || count < limits.min_completed) {
        result.failure = EvidenceFailure::no_coverage;
        return result;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& t = traces[i];
        if (t.ticket == 0 || t.actor_incarnation != limits.actor_incarnation ||
            t.generation != limits.generation) {
            result.failure = EvidenceFailure::invalid_identity;
            return result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (t.ticket == traces[j].ticket) {
                result.failure = EvidenceFailure::duplicate_ticket;
                return result;
            }
        }
        if (t.queue_depth > limits.max_queue_depth) {
            result.failure = EvidenceFailure::queue_limit;
            return result;
        }
        if (t.queue_depth > result.max_observed_queue_depth) {
            result.max_observed_queue_depth = t.queue_depth;
        }
        if (t.due_ns < t.armed_ns || t.deadline_ns < t.due_ns) {
            result.failure = EvidenceFailure::invalid_timestamp;
            return result;
        }
        if (t.canceled) {
            // A canceled ticket cannot enter user code. A notification may have
            // arrived before cancellation, but must not arrive after the ack.
            if (t.cancel_ack_ns == 0 || t.cancel_ack_ns < t.armed_ns ||
                t.user_start_ns != 0 ||
                t.user_finish_ns != 0 ||
                (t.notified_ns != 0 &&
                 (t.notified_ns < t.due_ns || t.notified_ns > t.cancel_ack_ns))) {
                result.failure = EvidenceFailure::invalid_cancellation;
                return result;
            }
            ++result.canceled;
            continue;
        }
        // Every non-canceled ticket needs all three timestamp boundaries;
        // a timely timer notification alone can never pass the contract.
        if (t.notified_ns < t.due_ns || t.user_start_ns < t.notified_ns ||
            t.user_finish_ns < t.user_start_ns || t.notified_ns == 0 ||
            t.user_start_ns == 0 || t.user_finish_ns == 0 ||
            t.cancel_ack_ns != 0) {
            result.failure = EvidenceFailure::invalid_timestamp;
            return result;
        }
        ++result.completed;
        const std::uint64_t delay = t.user_start_ns - t.notified_ns;
        const std::uint64_t due_delay = t.user_start_ns - t.due_ns;
        if (delay > result.max_notify_to_start_ns) {
            result.max_notify_to_start_ns = delay;
        }
        if (due_delay > result.max_due_to_start_ns) {
            result.max_due_to_start_ns = due_delay;
        }
        if (due_delay > limits.max_due_to_start_ns) {
            ++result.due_to_start_misses;
        }
        if (delay > limits.max_notify_to_start_ns) {
            ++result.start_delay_misses;
        }
        if (t.user_start_ns > t.deadline_ns) {
            ++result.deadline_misses;
        }
    }
    if (result.completed < limits.min_completed) {
        result.failure = EvidenceFailure::no_coverage;
    } else if (result.start_delay_misses != 0) {
        result.failure = EvidenceFailure::start_delay;
    } else if (result.due_to_start_misses != 0) {
        result.failure = EvidenceFailure::due_to_start;
    } else if (result.deadline_misses != 0) {
        result.failure = EvidenceFailure::deadline_miss;
    }
    return result;
}

} // namespace ores::cgl
