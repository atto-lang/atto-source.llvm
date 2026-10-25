#!/usr/bin/env python3
"""Exhaustive bounded dispatch identity check: stale replay cannot release current actor turn."""
from collections import namedtuple, deque

State = namedtuple('State', 'ticket worker running issued stopped')
FREE = -1
INITIAL = State(FREE, FREE, FREE, 0, False)
TICKETS = range(3)
WORKERS = range(2)


def transitions(s, unsafe=False):
    if not s.stopped and s.ticket == FREE:
        for t in TICKETS:
            if not s.issued & (1 << t):
                yield f'issue({t})', State(t, FREE, FREE, s.issued | (1 << t), s.stopped)
    if s.ticket != FREE and s.worker == FREE and not s.stopped:
        for w in WORKERS:
            yield f'enter({s.ticket},{w})', s._replace(worker=w, running=s.ticket)
    if s.worker != FREE and s.running == s.ticket:
        yield f'exit({s.ticket})', s._replace(ticket=FREE, worker=FREE, running=FREE)
    if s.ticket != FREE and s.worker == FREE:
        yield f'cancel_queued({s.ticket})', s._replace(ticket=FREE)
    if not s.stopped:
        yield 'stop', s._replace(stopped=True)
    for t in TICKETS:
        if (s.issued & (1 << t)) and t != s.ticket:
            # An old queued run or completion may be replayed at any point.
            # A correct implementation compares ticket identity before touching
            # current scheduling authority.
            if unsafe and s.ticket != FREE:
                yield f'UNSAFE_stale_release({t})', s._replace(ticket=FREE)
            else:
                yield f'ignore_stale({t})', s


def violation(s):
    if (s.worker == FREE) != (s.running == FREE):
        return 'physical worker and running-ticket disagree'
    if s.worker != FREE and (s.ticket == FREE or s.running != s.ticket):
        return 'obsolete ticket released an active newer dispatch'
    if s.ticket != FREE and not s.issued & (1 << s.ticket):
        return 'unissued ticket became active'
    return None


def explore(unsafe=False):
    q = deque([INITIAL]); parents = {INITIAL: None}
    while q:
        s = q.popleft()
        error = violation(s)
        if error:
            path = []
            while parents[s]:
                s, operation = parents[s]
                path.append(operation)
            return len(parents), error, list(reversed(path))
        for label, target in transitions(s, unsafe=unsafe):
            if target not in parents:
                parents[target] = (s, label)
                q.append(target)
    return len(parents), None, None


def main():
    count, bad, _ = explore()
    assert bad is None, bad
    n, error, path = explore(unsafe=True)
    assert error is not None and 'UNSAFE_stale_release' in path[-1]
    print(f'PASS: {count} dispatch-ticket states, no stale lease release')
    print(f'MUTATION DETECTED after {n} states: ' + ' -> '.join(path))


if __name__ == '__main__':
    main()
