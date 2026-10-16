"""Strict observable contract for the canonical Oreslang HTTP guest handler.

This checks the published fixture's arithmetic semantics, not the authenticity
of claimed remote execution. Provenance must still be checked independently.
"""
import re

COUNTER_CASE = re.compile(r"counter_response\(((?:0|-[1-9][0-9]*|[1-9][0-9]*))\)\Z")
REQUIRED_CASES = frozenset({
    "main()",
    "counter_response(-1)",
    "counter_response(0)",
    "counter_response(1)",
    "counter_response(2)",
})
# The 2026-10-10 guest execution matrix consists of main(), two signed edge
# inputs, and the complete 1..128 positive counter range. The smaller subset
# remains available to unit-test arithmetic/overflow independently of the
# full exact-source differential runtime gate.
FULL_MATRIX_CASES = frozenset({"main()", "counter_response(-1)", "counter_response(0)"}) | frozenset(
    f"counter_response({index})" for index in range(1, 129)
)
I64_MIN = -(1 << 63)
I64_MAX = (1 << 63) - 1


class CounterContractError(ValueError):
    pass


def verify_counter_observations(observed, *, require_full_matrix=False):
    """Reject equally-wrong backends, missing probes, and ambiguous evidence."""
    if type(observed) is not dict:
        raise CounterContractError("counter contract: missing observations")
    required = FULL_MATRIX_CASES if require_full_matrix else REQUIRED_CASES
    absent = required - observed.keys()
    if absent:
        raise CounterContractError("counter contract: missing required cases: " +
                                   ", ".join(sorted(absent)))
    if require_full_matrix:
        extra = observed.keys() - FULL_MATRIX_CASES
        if extra:
            raise CounterContractError(
                "counter contract: unexpected full-matrix cases: " + ", ".join(sorted(extra)))
        if len(observed) != len(FULL_MATRIX_CASES):
            raise CounterContractError("counter contract: full matrix case count mismatch")
    for case, value in observed.items():
        if type(case) is not str or type(value) is not int:
            raise CounterContractError("counter contract: invalid case or result")
        if case == "main()":
            expected = 43
        else:
            match = COUNTER_CASE.fullmatch(case)
            if not match:
                raise CounterContractError("counter contract: unknown case " + repr(case))
            argument = int(match.group(1))
            if not I64_MIN <= argument <= I64_MAX:
                raise CounterContractError("counter contract: argument outside signed i64")
            expected = argument * 43
            if not I64_MIN <= expected <= I64_MAX:
                raise CounterContractError("counter contract: arithmetic overflow probe")
        if value != expected:
            raise CounterContractError(
                f"counter contract: {case} expected {expected}, got {value}")
    return len(observed)
