#include "CglDispatchEvidence.hpp"
#include <array>
#include <cassert>
#include <cstdint>

using namespace ores::cgl;
using T = DispatchTrace;
static constexpr EvidenceLimits limits{17, 4, 20, 40, 8, 2, 2, 0, true};
static constexpr T ok1{17, 4, 1, 100, 105, 200, 110, 120, 124, 0, 2, false};
static constexpr T ok2{17, 4, 2, 200, 210, 310, 230, 250, 280, 0, 3, false};

int main() {
    const std::array<T, 2> normal{ok1, ok2};
    auto result = evaluate_dispatch_evidence(normal.data(), normal.size(), limits);
    assert(result.passes_observed_contract());
    assert(result.completed == 2 && result.canceled == 0);
    assert(result.max_notify_to_start_ns == 20);
    assert(result.max_due_to_start_ns == 40);
    assert(result.max_observed_queue_depth == 3);
    assert(!result.hard_realtime_qualified);

    // Notification is timely; actual user code starts after deadline.
    auto late_start = normal;
    late_start[1].user_start_ns = 311;
    late_start[1].user_finish_ns = 312;
    auto missed = evaluate_dispatch_evidence(late_start.data(), late_start.size(),
        EvidenceLimits{17, 4, 100, 120, 8, 2, 2, 0, true});
    assert(missed.failure == EvidenceFailure::deadline_miss);
    assert(missed.deadline_misses == 1);

    // Actual user code starts within deadline, but exceeds dispatch budget.
    auto after_due = normal;
    after_due[1].user_start_ns = 260;
    after_due[1].user_finish_ns = 270;
    auto due_missed = evaluate_dispatch_evidence(after_due.data(), after_due.size(),
        EvidenceLimits{17, 4, 50, 40, 8, 2, 2, 0, true});
    assert(due_missed.failure == EvidenceFailure::due_to_start);
    assert(due_missed.due_to_start_misses == 1);

    auto slow = normal;
    slow[1].user_start_ns = 270;
    slow[1].user_finish_ns = 280;
    auto delayed = evaluate_dispatch_evidence(slow.data(), slow.size(), limits);
    assert(delayed.failure == EvidenceFailure::start_delay);
    assert(delayed.start_delay_misses == 1);

    // A null/unobserved user start is never a passing timer-firing signal.
    auto missing = normal;
    missing[0].user_start_ns = 0;
    assert(evaluate_dispatch_evidence(missing.data(), missing.size(), limits).failure ==
        EvidenceFailure::invalid_timestamp);

    // Zero armed timestamp must not satisfy the missing first phase.
    auto unarmed = normal;
    unarmed[0].armed_ns = 0;
    assert(evaluate_dispatch_evidence(unarmed.data(), unarmed.size(), limits).failure ==
        EvidenceFailure::invalid_timestamp);

    auto early_notify = normal;
    early_notify[0].notified_ns = 104;
    assert(evaluate_dispatch_evidence(early_notify.data(), early_notify.size(), limits).failure ==
        EvidenceFailure::invalid_timestamp);

    auto duplicate = normal;
    duplicate[1].ticket = duplicate[0].ticket;
    assert(evaluate_dispatch_evidence(duplicate.data(), duplicate.size(), limits).failure ==
        EvidenceFailure::duplicate_ticket);

    auto wrong_generation = normal;
    wrong_generation[1].generation = 5;
    assert(evaluate_dispatch_evidence(wrong_generation.data(), wrong_generation.size(), limits).failure ==
        EvidenceFailure::invalid_identity);

    auto too_deep = normal;
    too_deep[1].queue_depth = 9;
    assert(evaluate_dispatch_evidence(too_deep.data(), too_deep.size(), limits).failure ==
        EvidenceFailure::queue_limit);

    const T canceled{17, 4, 3, 300, 305, 450, 310, 0, 0, 325, 1, true};
    const std::array<T, 3> with_cancel{ok1, ok2, canceled};
    auto with_cancel_limits = limits;
    with_cancel_limits.expected_total_records = 3;
    auto c = evaluate_dispatch_evidence(with_cancel.data(), with_cancel.size(),
                                        with_cancel_limits);
    assert(c.passes_observed_contract() && c.canceled == 1);

    auto canceled_unarmed = with_cancel;
    canceled_unarmed[2].armed_ns = 0;
    assert(evaluate_dispatch_evidence(canceled_unarmed.data(), canceled_unarmed.size(),
                                       with_cancel_limits).failure ==
        EvidenceFailure::invalid_timestamp);

    auto invalid_cancel = with_cancel;
    invalid_cancel[2].user_start_ns = 315;
    assert(evaluate_dispatch_evidence(invalid_cancel.data(), invalid_cancel.size(), with_cancel_limits).failure ==
        EvidenceFailure::invalid_cancellation);

    auto dropped = limits;
    dropped.dropped_records = 1;
    assert(evaluate_dispatch_evidence(normal.data(), normal.size(), dropped).failure ==
        EvidenceFailure::trace_loss);
    auto incomplete = limits;
    incomplete.capture_complete = false;
    assert(evaluate_dispatch_evidence(normal.data(), normal.size(), incomplete).failure ==
        EvidenceFailure::trace_loss);

    // The independent ticket ledger knows 3 winners but only 2 samples
    // arrived. The old minimum-completed rule incorrectly allowed this.
    auto silent_truncation = limits;
    silent_truncation.expected_total_records = 3;
    assert(evaluate_dispatch_evidence(normal.data(), normal.size(), silent_truncation).failure ==
        EvidenceFailure::trace_loss);
    auto impossible_config = limits;
    impossible_config.expected_total_records = 1; // less than min_completed=2
    assert(evaluate_dispatch_evidence(normal.data(), normal.size(), impossible_config).failure ==
        EvidenceFailure::invalid_config);

    auto only_one = evaluate_dispatch_evidence(normal.data(), 1, limits);
    assert(only_one.failure == EvidenceFailure::no_coverage);
    assert(evaluate_dispatch_evidence(nullptr, 0, limits).failure == EvidenceFailure::no_coverage);
    assert(evaluate_dispatch_evidence(normal.data(), normal.size(), EvidenceLimits{0, 4, 20, 40, 8, 2, 2, 0, true}).failure ==
        EvidenceFailure::invalid_config);

    auto out_of_order = normal;
    out_of_order[1].user_finish_ns = 249;
    assert(evaluate_dispatch_evidence(out_of_order.data(), out_of_order.size(), limits).failure ==
        EvidenceFailure::invalid_timestamp);
    return 0;
}
