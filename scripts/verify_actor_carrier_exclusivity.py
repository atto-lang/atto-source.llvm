#!/usr/bin/env python3
"""Exhaustively check a finite abstraction of OresVM per-actor carrier admission.

Model: two actors, two workers, two concurrent/reentrant jobs for actor A,
plus one job for actor B. Resume is a separate claim, never direct dispatch.
This bounded checker is useful evidence but NOT a full Java/LLVM proof.
"""
from collections import deque, namedtuple

State = namedtuple("State", "lease carriers jobs stopped")
# Jobs 0 and 1 are owned by actor 0; job 2 belongs to actor 1.
JOB_ACTOR = (0, 0, 1)
ACTORS, WORKERS = 2, 2
FREE, READY, RUNNING, SUSPENDED, DONE = -1, 0, 1, 2, 3
INITIAL = State((FREE, FREE), (FREE, FREE), (READY, READY, READY), (False, False))


def put(t, i, value):
    lst = list(t)
    lst[i] = value
    return tuple(lst)


def successors(s, unsafe_double_claim=False):
    for j, state in enumerate(s.jobs):
        a = JOB_ACTOR[j]
        if state == READY and not s.stopped[a]:
            for w, occupied in enumerate(s.carriers):
                # Admission under the actor's atomic lease is essential.
                if occupied == FREE and (s.lease[a] == FREE or unsafe_double_claim):
                    yield f"claim actor={a} job={j} worker={w}", State(
                        put(s.lease, a, w), put(s.carriers, w, j),
                        put(s.jobs, j, RUNNING), s.stopped)
        if state == RUNNING:
            for w, current in enumerate(s.carriers):
                if current == j:
                    for outcome, new_state in (("await", SUSPENDED), ("complete", DONE)):
                        yield f"{outcome} actor={a} job={j} worker={w}", State(
                            put(s.lease, a, FREE), put(s.carriers, w, FREE),
                            put(s.jobs, j, new_state), s.stopped)
        if state == SUSPENDED and not s.stopped[a]:
            yield f"wake actor={a} job={j}", s._replace(jobs=put(s.jobs, j, READY))
        if state in (READY, SUSPENDED):
            yield f"cancel actor={a} job={j}", s._replace(jobs=put(s.jobs, j, DONE))
    for a in range(ACTORS):
        if not s.stopped[a]:
            yield f"stop actor={a}", s._replace(stopped=put(s.stopped, a, True))


def check(s):
    # Independent observation of carriers, not merely checking the lease flag.
    executing = tuple(tuple(w for w, j in enumerate(s.carriers)
                            if j != FREE and JOB_ACTOR[j] == a)
                      for a in range(ACTORS))
    if any(len(ws) > 1 for ws in executing):
        return False, "two physical carriers executing same actor"
    if any((s.lease[a] == FREE) != (len(executing[a]) == 0)
           for a in range(ACTORS)):
        return False, "lease disagrees with physical executor"
    if any(ws and s.lease[a] != ws[0] for a, ws in enumerate(executing)):
        return False, "execution owner disagrees with lease"
    if any((s.jobs[j] == RUNNING) != (j in s.carriers)
           for j in range(len(s.jobs))):
        return False, "task run state disagrees with carrier occupancy"
    return True, ""


def explore(unsafe=False):
    queue = deque([INITIAL]); seen = {INITIAL: None}; witness = None
    while queue:
        s = queue.popleft()
        ok, error = check(s)
        if not ok:
            path = []
            while seen[s] is not None:
                s, step = seen[s]
                path.append(step)
            path.reverse()
            return len(seen), (error, path), witness
        # Reentrancy witness: job 0 suspended, job 1 running on same actor.
        if witness is None and s.jobs[0] == SUSPENDED and s.jobs[1] == RUNNING:
            path = []; cursor = s
            while seen[cursor] is not None:
                cursor, step = seen[cursor]
                path.append(step)
            witness = list(reversed(path))
        for action, next_state in successors(s, unsafe_double_claim=unsafe):
            if next_state not in seen:
                seen[next_state] = (s, action)
                queue.append(next_state)
    return len(seen), None, witness


def main():
    n, violation, witness = explore()
    assert violation is None, violation
    assert witness, "model never demonstrated real reentrant interleaving"
    print(f"PASS: {n} reachable states, all satisfy exclusive physical execution")
    print("Reentrant witness: " + " -> ".join(witness))
    n_bad, violation_bad, _ = explore(unsafe=True)
    assert violation_bad and "two physical carriers" in violation_bad[0], violation_bad
    print(f"MUTATION DETECTED after {n_bad} states: {violation_bad[0]}")
    print("Counterexample: " + " -> ".join(violation_bad[1]))


if __name__ == "__main__":
    main()
