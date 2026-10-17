#!/usr/bin/env python3
"""Abstract, finite physical-grant model; NOT runtime refinement evidence.

A grant has a unique serial, incarnation and carrier identity. The model bounds
serials/epochs solely to make complete state exploration terminate. Exhaustion
is fail-closed, never wrapping/aliasing existing physical permits.
"""
from collections import deque
from dataclasses import dataclass

WORKERS = range(4)
MAX_SERIAL = 5
MAX_GENERATION = 2


@dataclass(frozen=True)
class Ticket:
    generation: int
    serial: int
    worker: int


@dataclass(frozen=True)
class State:
    generation: int = 1
    next_serial: int = 1
    holders: frozenset[Ticket] = frozenset()
    sealed: bool = False


def claim(state: State, worker: int, cap: int, *, guard_capacity=True,
          guard_sealed=True):
    if (guard_sealed and state.sealed) or state.next_serial > MAX_SERIAL:
        return None
    if any(ticket.worker == worker for ticket in state.holders):
        return None
    if guard_capacity and len(state.holders) >= cap:
        return None
    ticket = Ticket(state.generation, state.next_serial, worker)
    return State(state.generation, state.next_serial + 1,
                 state.holders | {ticket}, state.sealed), ticket


def release(state: State, ticket: Ticket, worker: int, *, guard_serial=True):
    if ticket.generation != state.generation or ticket.worker != worker:
        return state
    if guard_serial:
        if ticket not in state.holders:
            return state
        return State(state.generation, state.next_serial,
                     state.holders - {ticket}, state.sealed)
    # Deliberately broken variant: only carrier identity is checked; permits ABA.
    matches = {holder for holder in state.holders if holder.worker == worker}
    return State(state.generation, state.next_serial,
                 state.holders - matches, state.sealed)


def stop(state: State):
    return State(state.generation, state.next_serial, state.holders, True)


def restart(state: State):
    if not state.sealed or state.holders or state.generation >= MAX_GENERATION:
        return None
    return State(state.generation + 1, 1, frozenset(), False)


def successors(state: State, cap: int, *, guard_capacity=True):
    for worker in WORKERS:
        admitted = claim(state, worker, cap, guard_capacity=guard_capacity)
        if admitted is not None:
            yield f"claim({worker})", admitted[0]
    for ticket in state.holders:
        yield f"release({ticket.serial},{ticket.worker})", release(state, ticket, ticket.worker)
    if not state.sealed:
        yield "seal", stop(state)
    newer = restart(state)
    if newer is not None:
        yield "restart", newer


def explore(cap: int, *, guard_capacity=True):
    initial = State()
    pending = deque([(initial, ())])
    seen = {initial}
    while pending:
        state, path = pending.popleft()
        if len(state.holders) > cap:
            return len(seen), path
        if len({t.worker for t in state.holders}) != len(state.holders):
            raise AssertionError("duplicate physical carrier owner")
        if any(t.generation != state.generation for t in state.holders):
            raise AssertionError("wrong-incarnation grant")
        if state.sealed and claim(state, 99, cap) is not None:
            raise AssertionError("sealed actor admitted a new carrier")
        for action, nxt in successors(state, cap, guard_capacity=guard_capacity):
            if nxt not in seen:
                seen.add(nxt)
                pending.append((nxt, path + (action,)))
    return len(seen), None


def assert_replay_and_retirement():
    initial = State()
    first, stale = claim(initial, 0, 3)
    released = release(first, stale, 0)
    next_state, current = claim(released, 0, 3)
    assert current.serial != stale.serial
    assert release(next_state, stale, 0) == next_state
    assert release(next_state, stale, 1) == next_state
    assert release(next_state, stale, 0, guard_serial=False) != next_state
    assert release(next_state, current, 0).holders == frozenset()
    stopped = stop(next_state)
    assert claim(stopped, 1, 3) is None
    assert restart(stopped) is None
    quiescent = release(stopped, current, 0)
    restarted = restart(quiescent)
    assert restarted is not None
    newer, successor = claim(restarted, 0, 3)
    assert release(newer, current, 0) == newer
    assert release(newer, successor, 0).holders == frozenset()
    assert claim(initial, 0, 0) is None
    return "PASS stale/double/cross-carrier release, same-carrier ABA, sealed retirement, incarnation fencing"



def explore_cross_actor_carriers(*, allow_nested=False):
    """Explore two distinct actor incarnations sharing four platform carriers.

    A platform carrier must not own simultaneous grants for separate actors,
    even when the individual actor capacity limits would allow it.
    """
    initial = frozenset()
    pending = deque([(initial, ())])
    seen = {initial}
    caps = {"A": 2, "B": 3}
    while pending:
        owners, trace = pending.popleft()
        carriers = [worker for _, worker in owners]
        if len(set(carriers)) != len(carriers):
            return len(seen), trace
        for actor in ("A", "B"):
            if sum(a == actor for a, _ in owners) >= caps[actor]:
                continue
            for worker in WORKERS:
                pair = (actor, worker)
                if pair in owners:
                    continue
                if not allow_nested and worker in carriers:
                    continue
                newer = owners | {pair}
                if newer not in seen:
                    seen.add(newer)
                    pending.append((newer, trace + (f"{actor}:claim({worker})",)))
        for pair in owners:
            newer = owners - {pair}
            if newer not in seen:
                seen.add(newer)
                pending.append((newer, trace + (f"{pair[0]}:release({pair[1]})",)))
    return len(seen), None


def main():
    if not __debug__:
        raise SystemExit("actor grant contract tests must not run with Python -O")
    for cap in (1, 2, 3):
        states, violation = explore(cap)
        if violation:
            raise AssertionError(f"capacity {cap} violated after {violation}")
        print(f"PASS cap={cap} finite states={states}")
    print(assert_replay_and_retirement())
    states, violation = explore_cross_actor_carriers()
    if violation is not None:
        raise AssertionError(f"platform carrier admitted by multiple actors: {violation}")
    print(f"PASS two-actor shared carrier discipline: {states} finite states")
    states, violation = explore_cross_actor_carriers(allow_nested=True)
    if violation is None:
        raise AssertionError("cross-actor carrier exclusion mutation was not detected")
    print(f"PASS cross-actor exclusion mutation: {len(violation)}-step counterexample")
    states, violation = explore(3, guard_capacity=False)
    if violation is None:
        raise AssertionError("capacity guard-removal mutation was not caught")
    print(f"PASS capacity-guard mutation: {len(violation)}-step counterexample")
    print("LIMIT: abstract protocol only; Java/LLVM scheduler integration unverified")


if __name__ == '__main__':
    main()
