#!/usr/bin/env python3
"""Bounded, incarnation-fenced physical actor-grant model (not production proof).

A grant is minted once per physical segment, including a continuation resuming
on the same carrier. Never mistake a carrier identity for a lease identity.
This intentionally does not prove logical ordering or actor-state safety.
"""
from collections import deque
from dataclasses import dataclass

WORKERS = range(4)
MAX_SERIAL = 5
MAX_GENERATION = 2


@dataclass(frozen=True, order=True)
class Grant:
    generation: int
    serial: int
    owner: int


@dataclass(frozen=True)
class State:
    generation: int
    next_serial: int
    holders: frozenset[Grant]
    stopping: bool


def claim(s: State, owner: int, capacity: int, *, check_capacity=True) -> tuple[State, Grant] | None:
    if owner not in WORKERS or s.stopping or s.next_serial > MAX_SERIAL:
        return None
    if any(g.owner == owner for g in s.holders):
        return None
    if check_capacity and len(s.holders) >= capacity:
        return None
    ticket = Grant(s.generation, s.next_serial, owner)
    return State(s.generation, s.next_serial + 1, s.holders | {ticket}, False), ticket


def release(s: State, ticket: Grant, releasing_owner: int) -> State:
    # Generation, nonce, and executing owner must ALL match. A previous turn
    # on this same physical worker has no authority over its next turn.
    if ticket.generation != s.generation or ticket.owner != releasing_owner:
        return s
    if ticket not in s.holders:
        return s
    return State(s.generation, s.next_serial, s.holders - {ticket}, s.stopping)


def stop(s: State) -> State:
    return State(s.generation, s.next_serial, s.holders, True)


def restart(s: State) -> State | None:
    if not s.stopping or s.holders or s.generation == MAX_GENERATION:
        return None
    return State(s.generation + 1, 1, frozenset(), False)


def successors(s: State, capacity: int, check_capacity: bool):
    for owner in WORKERS:
        next_claim = claim(s, owner, capacity, check_capacity=check_capacity)
        if next_claim is not None:
            yield f'claim({owner})', next_claim[0]
    for g in s.holders:
        yield f'release({g.serial})', release(s, g, g.owner)
    if not s.stopping:
        yield 'stop', stop(s)
    next_gen = restart(s)
    if next_gen is not None:
        yield 'restart', next_gen


def check_invariants(s: State, capacity: int) -> None:
    assert len(s.holders) <= capacity, 'physical cap exceeded'
    assert len({g.owner for g in s.holders}) == len(s.holders), 'owner double-admitted'
    assert len({g.serial for g in s.holders}) == len(s.holders), 'grant nonce reused'
    assert all(g.generation == s.generation and 1 <= g.serial < s.next_serial
               for g in s.holders), 'stale or invalid grant executing'


def explore(capacity: int, *, check_capacity=True) -> tuple[int, tuple[str, ...] | None]:
    initial = State(1, 1, frozenset(), False)
    todo = deque([(initial, ())])
    seen = {initial}
    while todo:
        state, trace = todo.popleft()
        try:
            check_invariants(state, capacity)
        except AssertionError:
            return len(seen), trace
        for action, successor in successors(state, capacity, check_capacity):
            if successor not in seen:
                seen.add(successor)
                todo.append((successor, trace + (action,)))
    return len(seen), None


def targeted_regressions():
    original = State(1, 1, frozenset(), False)
    admitted, old = claim(original, 0, 1)
    assert claim(admitted, 0, 3) is None, 'nested owner acquired second slot'
    freed = release(admitted, old, 0)
    replacement, fresh = claim(freed, 0, 1)
    assert old != fresh
    assert release(replacement, old, 0) == replacement, 'same-carrier stale ticket ABA'
    assert release(replacement, fresh, 1) == replacement, 'wrong carrier released ticket'
    assert release(replacement, Grant(9, fresh.serial, fresh.owner), 0) == replacement
    stopped = stop(replacement)
    assert claim(stopped, 1, 3) is None, 'post-stop admission allowed'
    assert restart(stopped) is None, 'retired while active grant exists'
    terminal = release(stopped, fresh, 0)
    successor = restart(terminal)
    assert successor is not None and successor.generation == 2
    assert release(successor, fresh, 0) == successor, 'prior incarnation ticket accepted'
    assert claim(successor, 0, 1) is not None, 'new incarnation admission unavailable'
    # A mutant treating only carrier equality as authority would erroneously
    # remove `fresh` when asked to release `old`.
    carrier_only_release = State(replacement.generation, replacement.next_serial,
                                 frozenset(), replacement.stopping)
    assert carrier_only_release != release(replacement, old, 0), 'ABA mutation undetected'


def main():
    if not __debug__:
        raise RuntimeError('actor contract checks require Python assertions')
    targeted_regressions()
    print('PASS targeted ABA, carrier, stop, restart, and generation fencing')
    for cap in (1, 2, 3):
        count, trace = explore(cap)
        assert trace is None, (cap, trace)
        print(f'PASS bounded physical admission cap={cap}: {count} states')
    _, mutation = explore(3, check_capacity=False)
    assert mutation is not None and len(mutation) >= 4, 'removed admission guard not detected'
    print(f'PASS capacity-guard mutation detected after {len(mutation)} transitions')
    print('LIMIT: abstract physical protocol only; no state effect / real scheduler proof')


if __name__ == '__main__':
    main()
