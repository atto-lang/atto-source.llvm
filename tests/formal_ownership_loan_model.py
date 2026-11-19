#!/usr/bin/env python3
"""Independent exhaustive finite-state verifier for NativeOwnershipLoans.tla.

This is a model-level safety test, not LLVM source/IR refinement evidence.
"""
from collections import deque

LOANS = (0, 1)
INIT = ("owned", frozenset(), None, False)


def invariant(state):
    owner, readers, writer, suspended = state
    return (owner in ("owned", "moved")
            and all(x in LOANS for x in readers)
            and (writer is None or writer in LOANS)
            and (writer is None or not readers)
            and (owner != "moved" or (not readers and writer is None))
            and (not suspended or (not readers and writer is None)))


def successors(state):
    owner, readers, writer, suspended = state
    if owner == "owned" and not suspended:
        if writer is None:
            for loan in LOANS:
                if loan not in readers:
                    yield ("owned", readers | {loan}, None, False)
            if not readers:
                for loan in LOANS:
                    yield ("owned", readers, loan, False)
        if not readers and writer is None:
            yield ("moved", readers, writer, False)
            yield ("owned", readers, writer, True)
    for loan in readers:
        yield (owner, readers - {loan}, writer, suspended)
    if writer is not None:
        yield (owner, readers, None, suspended)
    if owner == "moved":
        yield ("owned", readers, writer, suspended)
    if suspended:
        yield (owner, readers, writer, False)


def explore(transitions):
    queue = deque([INIT])
    seen = {INIT}
    while queue:
        state = queue.popleft()
        if not invariant(state):
            return state, len(seen)
        for child in transitions(state):
            if child not in seen:
                seen.add(child)
                queue.append(child)
    return None, len(seen)


def main():
    violation, explored = explore(successors)
    if violation is not None or explored < 5:
        raise AssertionError(f"original model violated safety: {violation}, states={explored}")

    # Mutation test: a buggy suspend that ignores live loans MUST be detected.
    def faulty(state):
        yield from successors(state)
        owner, readers, writer, suspended = state
        if owner == "owned" and not suspended and readers:
            yield (owner, readers, writer, True)

    counterexample, _ = explore(faulty)
    if counterexample is None:
        raise AssertionError("negative-control mutation escaped the model checker")
    print(f"native ownership abstract model: PASS {explored} states; negative control detected")


if __name__ == "__main__":
    main()
