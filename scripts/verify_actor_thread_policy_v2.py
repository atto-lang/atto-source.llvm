#!/usr/bin/env python3
"""Finite model for proposed v2 bounded physical actor execution grants.

Host-only contract model, not a proof of Java or LLVM scheduler refinement.
It deliberately models physical permits, generation fencing and teardown,
not actor-state ownership/effect correctness or mailbox sequencing.
"""
from collections import deque
from dataclasses import dataclass

WORKERS = range(4)
LAST_GENERATION = 3

@dataclass(frozen=True)
class State:
    generation: int
    holders: frozenset[tuple[int, int]]
    closing: bool


def claim(state: State, worker: int, cap: int, *, enforce_cap=True) -> State | None:
    identity = (state.generation, worker)
    if state.closing or identity in state.holders:
        return None
    if enforce_cap and len(state.holders) >= cap:
        return None
    return State(state.generation, state.holders | {identity}, state.closing)


def release(state: State, owner_generation: int, worker: int) -> State:
    identity = (owner_generation, worker)
    # Foreign or retired-generation releases cannot affect current permits.
    if owner_generation != state.generation or identity not in state.holders:
        return state
    return State(state.generation, state.holders - {identity}, state.closing)


def edges(state: State, cap: int, *, enforce_cap=True):
    for worker in WORKERS:
        next_state = claim(state, worker, cap, enforce_cap=enforce_cap)
        if next_state is not None:
            yield f"claim({worker})", next_state
    for generation, worker in state.holders:
        yield f"release({generation},{worker})", release(state, generation, worker)
    if not state.closing:
        yield "stop", State(state.generation, state.holders, True)
    if state.closing and not state.holders and state.generation < LAST_GENERATION:
        yield "restart", State(state.generation + 1, frozenset(), False)


def explore(cap: int, *, enforce_cap=True):
    initial = State(1, frozenset(), False)
    pending = deque([(initial, ())])
    seen = {initial}
    while pending:
        state, trace = pending.popleft()
        if len(state.holders) > cap:
            return len(seen), trace
        if state.generation > 1 and release(state, state.generation - 1, 0) != state:
            raise AssertionError("stale owner release changed current generation")
        for action, successor in edges(state, cap, enforce_cap=enforce_cap):
            if successor not in seen:
                seen.add(successor)
                pending.append((successor, trace + (action,)))
    return len(seen), None


def main():
    if not __debug__:
        raise RuntimeError("contract checks cannot run with Python assertions disabled")
    for cap in (1, 2, 3):
        states, violation = explore(cap)
        if violation is not None:
            raise AssertionError(f"cap {cap} violated: {violation}")
        print(f"PASS v2 abstract cap {cap}: {states} reachable states")
    states, violation = explore(3, enforce_cap=False)
    if violation is None:
        raise AssertionError("mutation removing slot capacity guard was not detected")
    print(f"PASS guard-removal mutation: over-cap reached in {len(violation)} steps")
    print("LIMIT: abstract model only; actor field ownership and production scheduler are unverified")


if __name__ == "__main__":
    main()
