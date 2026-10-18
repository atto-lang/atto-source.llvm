#!/usr/bin/env python3
"""Deterministic model for cross-context logical actor route publication.

Source-independent oracle only: it does not execute Java/Graal/LLVM actor code.
It models route pins, paused host sends, checkpoint rollback, commit and old
physical retirement. Invariants use explicit checks so python -O is equivalent.
"""
from dataclasses import dataclass, field
import random

def require(ok, message):
    if not ok:
        raise AssertionError(message)

@dataclass
class Generation:
    ident: int
    pins: int = 0
    closed: bool = False

    def pin(self):
        require(not self.closed, "pinning closed generation")
        self.pins += 1

    def unpin(self):
        require(self.pins > 0, "generation pin underflow")
        self.pins -= 1

    def retire(self):
        require(self.pins == 0, "retired generation while route-pinned")
        self.closed = True

@dataclass
class Physical:
    generation: Generation
    state: int
    code: int
    mailbox: list = field(default_factory=list)
    alive: bool = True

    def send(self, ticket, delta):
        require(self.alive, "send to dead physical actor")
        self.mailbox.append((ticket, delta))

    def drain_one(self):
        if not self.mailbox:
            return False
        ticket, delta = self.mailbox.pop(0)
        self.state += delta if self.code == 1 else abs(delta)
        return ticket

@dataclass
class Router:
    active: Physical
    logical_id: int = 0xA670
    incarnation: int = 1
    route_epoch: int = 1
    upgrading: bool = False
    accepted: int = 0
    delivered: int = 0
    blocked: list = field(default_factory=list)
    retiring: list = field(default_factory=list)

    def __post_init__(self):
        self.active.generation.pin()

    def send(self, delta):
        self.accepted += 1
        msg = (self.accepted, delta)
        if self.upgrading:
            # Real Java v1 blocks the HOST caller rather than buffering the
            # mutable graph. The model records the eventual admission so
            # conservation can be checked without threads.
            self.blocked.append(msg)
        else:
            self.active.send(*msg)

    def process(self):
        ticket = self.active.drain_one()
        if ticket is False:
            return False
        require(ticket == self.delivered + 1,
                "lost, duplicated or reordered logical message ticket")
        self.delivered += 1
        return True

    def begin_upgrade(self):
        if self.upgrading or self.active.mailbox:
            return None
        self.upgrading = True
        return (self.active, self.route_epoch, self.active.state)

    def finish_upgrade(self, token, candidate, compatible=True,
                       restore_ok=True, publish_ok=True):
        require(self.upgrading, "finishing absent upgrade")
        old, expected_route_epoch, checkpoint = token
        require(old is self.active and expected_route_epoch == self.route_epoch,
                "stale route token")
        committed = False
        if compatible and restore_ok and publish_ok:
            candidate.generation.pin()
            candidate.state = checkpoint
            self.active = candidate
            self.route_epoch += 1
            old.alive = False
            self.retiring.append(old)
            committed = True

        self.upgrading = False
        # All host calls that waited at the Java route monitor now linearize
        # after rollback/publication against the authoritative route.
        for ticket, delta in self.blocked:
            self.active.send(ticket, delta)
        self.blocked.clear()
        return committed

    def reap(self):
        kept = []
        for old in self.retiring:
            if not old.alive and not old.mailbox:
                old.generation.unpin()
            else:
                kept.append(old)
        self.retiring = kept

    def verify(self):
        require(self.logical_id == 0xA670, "logical actor identity changed")
        require(self.incarnation == 1,
                "transparent hot upgrade changed logical actor incarnation")
        require(self.route_epoch >= 1, "invalid route epoch")
        require(self.active.alive, "authoritative route points at dead actor")
        require(self.active.generation.pins >= 1,
                "active route lost its generation pin")
        pending = len(self.active.mailbox) + len(self.blocked)
        require(self.accepted == self.delivered + pending,
                "logical accepted-message conservation violated")
        for old in self.retiring:
            require(old.generation.pins >= 1,
                    "old generation unpinned before physical retirement")

def main():
    transitions = 0
    commits = 0
    rollbacks = 0
    retire_denials = 0

    for seed in range(64):
        rng = random.Random(0xC0A57 + seed)
        generations = [Generation(i) for i in range(1, 257)]
        next_gen = 1
        router = Router(Physical(generations[0], 0, 1))

        for _ in range(2500):
            action = rng.randrange(7)
            if action <= 2:
                router.send(rng.randrange(-5, 6))
            elif action <= 4:
                router.process()
            else:
                token = router.begin_upgrade()
                if token is not None:
                    # Model host calls arriving while Java's route monitor is
                    # paused. They linearize only after commit/rollback.
                    for _ in range(rng.randrange(4)):
                        router.send(rng.randrange(-5, 6))
                if token is not None and next_gen < len(generations):
                    candidate = Physical(
                        generations[next_gen],
                        -999999,
                        2 if router.active.code == 1 else 1)
                    next_gen += 1
                    compatible = rng.randrange(7) != 0
                    restore_ok = rng.randrange(6) != 0
                    publish_ok = rng.randrange(9) != 0
                    if router.finish_upgrade(
                            token, candidate, compatible, restore_ok, publish_ok):
                        commits += 1
                    else:
                        rollbacks += 1
                elif token is not None:
                    router.finish_upgrade(
                        token,
                        Physical(Generation(1000000 + transitions), -1, 2),
                        compatible=False)
                    rollbacks += 1

            # Active generation retirement must always fail.
            try:
                router.active.generation.retire()
                raise AssertionError("active generation retirement unexpectedly succeeded")
            except AssertionError as failure:
                if str(failure).startswith("active generation retirement"):
                    raise
                retire_denials += 1

            router.reap()
            router.verify()
            transitions += 1

        while router.active.mailbox:
            router.process()
        router.reap()
        router.verify()
        require(router.accepted == router.delivered,
                "final logical message loss/duplication")
        router.active.alive = False
        router.active.generation.unpin()
        router.active.generation.retire()

    print(
        "PASS:",
        transitions,
        "logical-route transitions,",
        commits,
        "commits,",
        rollbacks,
        "rollbacks,",
        retire_denials,
        "active-retirement denials")

if __name__ == "__main__":
    main()
