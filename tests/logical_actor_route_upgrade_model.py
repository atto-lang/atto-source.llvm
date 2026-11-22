#!/usr/bin/env python3
"""Deterministic model for stable logical ActorRef routing across hot upgrades.

Model-only: this does not execute Oreslang/Java/native runtime code. It focuses
on the route-lock contract: a logical send is admitted to exactly one physical
incarnation; sends that arrive during migration wait and are released only
after rollback or publication; old physical refs become permanently stale.
"""
from collections import deque
from dataclasses import dataclass, field
import random

def require(ok, msg):
    if not ok:
        raise AssertionError(msg)

@dataclass
class Physical:
    ident: int
    code: int
    state: int
    queue: deque = field(default_factory=deque)
    stopped: bool = False
    sealed: bool = False

@dataclass
class Model:
    logical_incarnation: int = 1
    route_epoch: int = 1
    next_physical: int = 2
    active: Physical = field(default_factory=lambda: Physical(1, 1, 0))
    retired: list = field(default_factory=list)
    upgrading: bool = False
    waiting: deque = field(default_factory=deque)
    next_ticket: int = 1
    returned_sends: int = 0
    processed: int = 0
    rejected_stale: int = 0
    committed_upgrades: int = 0
    rolled_back_upgrades: int = 0
    expected_sum: int = 0
    accepted_seen: set = field(default_factory=set)
    processed_seen: set = field(default_factory=set)
    last_accepted_ticket: int = 0
    last_processed_ticket: int = 0

    def logical_send(self, delta):
        ticket = self.next_ticket
        self.next_ticket += 1
        if self.upgrading:
            self.waiting.append((ticket, delta))
            return
        self._admit(self.active, ticket, delta)

    def _admit(self, physical, ticket, delta):
        require(not physical.stopped, "logical route admitted to stopped physical actor")
        require(not physical.sealed, "logical route admitted to sealed physical actor")
        physical.queue.append((ticket, delta))
        require(ticket not in self.accepted_seen, "duplicate logical send admission")
        require(ticket > self.last_accepted_ticket, "logical send admission reordered")
        self.accepted_seen.add(ticket)
        self.last_accepted_ticket = ticket
        self.returned_sends += 1

    def process_one(self):
        if not self.active.queue:
            return False
        ticket, delta = self.active.queue.popleft()
        if self.active.code == 1:
            self.active.state += delta
            self.expected_sum += delta
        else:
            self.active.state += abs(delta)
            self.expected_sum += abs(delta)
        require(ticket in self.accepted_seen, "processed message was never admitted")
        require(ticket not in self.processed_seen, "duplicate message processing")
        require(ticket > self.last_processed_ticket, "logical FIFO ordering changed")
        self.processed_seen.add(ticket)
        self.last_processed_ticket = ticket
        self.processed += 1
        return True

    def begin_upgrade(self):
        require(not self.upgrading, "nested logical upgrade")
        self.upgrading = True
        self.active.sealed = True

    def finish_upgrade(self, compatible=True, restore_ok=True):
        require(self.upgrading, "finish without upgrade")
        old = self.active

        # Runtime transfer is nonblocking at safe point. Any old accepted mail
        # causes rollback; sends waiting behind logical routeLock are NOT yet
        # admitted to either physical actor.
        if old.queue or not compatible or not restore_ok:
            old.sealed = False
            self.upgrading = False
            self.rolled_back_upgrades += 1
            self._release_waiters(old)
            return False

        candidate = Physical(self.next_physical, 2 if old.code == 1 else 1, old.state)
        self.next_physical += 1

        # Commit point: logical route changes before old physical retirement.
        self.active = candidate
        self.route_epoch += 1
        self.committed_upgrades += 1
        old.stopped = True
        old.sealed = True
        self.retired.append(old)
        self.upgrading = False
        self._release_waiters(candidate)
        return True

    def _release_waiters(self, physical):
        while self.waiting:
            ticket, delta = self.waiting.popleft()
            self._admit(physical, ticket, delta)

    def stale_send(self):
        if not self.retired:
            return
        old = self.retired[-1]
        require(old.stopped and old.sealed, "retired physical ref became writable")
        self.rejected_stale += 1

    def verify(self):
        require(self.logical_incarnation == 1, "transparent hot upgrade changed logical incarnation")
        require(self.route_epoch == self.committed_upgrades + 1, "route epoch drift")
        require(self.returned_sends == len(self.accepted_seen), "send return/admission mismatch")
        require(self.processed == len(self.processed_seen), "processed-ticket accounting mismatch")
        queued = sum(len(p.queue) for p in [self.active] + self.retired)
        require(self.returned_sends == self.processed + queued,
                "accepted-message conservation violated")
        for old in self.retired:
            require(old.stopped and old.sealed, "retired physical actor is writable")
            require(not old.queue, "published upgrade retired actor with accepted mail")
        if not self.upgrading:
            require(not self.waiting, "waiters leaked after route reopened")
            require(not self.active.sealed, "active route remained sealed")
        else:
            require(self.active.sealed, "migration did not seal old physical route")

def one(seed):
    rng = random.Random(seed)
    m = Model()
    transitions = 0
    for _ in range(5000):
        action = rng.randrange(10)
        if action <= 3:
            m.logical_send(rng.randrange(-5, 6))
        elif action <= 5:
            m.process_one()
        elif action == 6 and not m.upgrading:
            m.begin_upgrade()
        elif action == 7 and m.upgrading:
            m.finish_upgrade(
                compatible=rng.randrange(7) != 0,
                restore_ok=rng.randrange(9) != 0)
        elif action == 8:
            m.stale_send()
        elif action == 9 and m.upgrading:
            # Deterministic rollback/failure path.
            m.finish_upgrade(compatible=False)
        m.verify()
        transitions += 1

    if m.upgrading:
        m.finish_upgrade(compatible=False)
    while m.active.queue:
        m.process_one()
        m.verify()

    require(not m.waiting, "final waiter leak")
    require(m.returned_sends == m.processed, "final accepted message lost")
    require(m.last_processed_ticket == m.returned_sends,
            "logical FIFO/ticket sequence did not fully drain")
    require(m.active.state == m.expected_sum,
            "state/checkpoint evolution diverged from exactly-once delivery")
    m.verify()
    return transitions, m.committed_upgrades, m.rolled_back_upgrades, m.rejected_stale

def main():
    transitions = committed = rolled = stale = 0
    for i in range(64):
        a,b,c,d = one(0xA670_5000 + i)
        transitions += a
        committed += b
        rolled += c
        stale += d
    print("PASS:",
          transitions, "route transitions;",
          committed, "committed upgrades;",
          rolled, "rollbacks;",
          stale, "stale-physical sends rejected")

if __name__ == "__main__":
    main()
