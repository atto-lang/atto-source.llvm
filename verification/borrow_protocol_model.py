#!/usr/bin/env python3
"""Finite-state ownership/borrow protocol checker, with adversarial mutations.

This proves invariants of the BOUNDED ABSTRACT MODEL below, not the
correctness of either compiler implementation. Source-level tests must
separately establish refinement for LLVM and GraalVM.
"""
from collections import deque
from dataclasses import dataclass, replace

SHARED_LIMIT = 2
MUTABLE = (True, False)  # one mutable root and one immutable root


@dataclass(frozen=True)
class Root:
    live: bool = True
    shared: int = 0
    exclusive: bool = False


@dataclass(frozen=True)
class State:
    roots: tuple[Root, Root] = (Root(), Root())
    suspended: bool = False


def safe(s):
    return all(0 <= x.shared <= SHARED_LIMIT
               and (x.live or (x.shared == 0 and not x.exclusive))
               and not (x.exclusive and x.shared)
               for x in s.roots) and (not s.suspended or all(
                   x.shared == 0 and not x.exclusive for x in s.roots))


def successors(s, mutation=None):
    if s.suspended:
        yield "resume", replace(s, suspended=False)
        return
    for i, root in enumerate(s.roots):
        def update(op, new_root):
            roots = list(s.roots)
            roots[i] = new_root
            return op + str(i), replace(s, roots=tuple(roots))

        if root.live and not root.exclusive and root.shared < SHARED_LIMIT:
            yield update("borrow_shared_", replace(root, shared=root.shared + 1))
        if root.live and MUTABLE[i] and not root.exclusive and (
                root.shared == 0 or mutation == "double_borrow"):
            yield update("borrow_exclusive_", replace(root, exclusive=True))
        if root.shared:
            yield update("release_shared_", replace(root, shared=root.shared - 1))
        if root.exclusive:
            yield update("release_exclusive_", replace(root, exclusive=False))
        if root.live and (not root.exclusive and not root.shared
                          or mutation == "move_live_loan"):
            yield update("move_", replace(root, live=False))
        if not root.live and MUTABLE[i]:
            yield update("reinitialize_", replace(root, live=True))
    if all(x.shared == 0 and not x.exclusive for x in s.roots) or mutation == "yield_live_loan":
        yield "suspend", replace(s, suspended=True)


def model_check(mutation=None):
    initial = State()
    pending = deque([initial])
    paths = {initial: ()}
    transitions = 0
    while pending:
        s = pending.popleft()
        if not safe(s):
            return len(paths), transitions, paths[s]
        for action, nxt in successors(s, mutation):
            transitions += 1
            if nxt not in paths:
                paths[nxt] = paths[s] + (action,)
                pending.append(nxt)
    return len(paths), transitions, None


def main():
    states, transitions, counterexample = model_check()
    assert counterexample is None, f"SAFE model violated: {counterexample}"
    assert states >= 20 and transitions >= 50, "State graph is unexpectedly trivial"
    print(f"SAFE: exhausted {states} reachable states and {transitions} transitions")
    for mutation in ("double_borrow", "move_live_loan", "yield_live_loan"):
        states, transitions, counterexample = model_check(mutation)
        assert counterexample, f"{mutation}: mutation escaped detection"
        print(f"EXPECTED UNSAFE {mutation}: {' -> '.join(counterexample)}")


if __name__ == "__main__":
    main()
