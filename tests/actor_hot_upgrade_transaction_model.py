#!/usr/bin/env python3
"""Source-free, deterministic actor hot-upgrade interleaving oracle.

This is a state-machine MODEL, not runtime source tests or compiler execution.
It checks admitted-message conservation and same-schema rollback semantics.
"""
import collections
import dataclasses
import random

def require(condition, message):
    if not condition:
        raise AssertionError(message)

@dataclasses.dataclass
class Model:
    actor_id: int = 1
    epoch: int = 1
    code: int = 1
    state: int = 0
    frozen: bool = False
    pins: int = 0
    accepted: int = 0
    delivered: int = 0
    mail: object = dataclasses.field(default_factory=collections.deque)
    in_flight: object = None
    published: int = 0
    rejected: int = 0
    rolled_back: int = 0

    def send(self, value):
        if self.frozen:
            self.rejected += 1
            return False
        self.accepted += 1
        self.mail.append((self.accepted, value))
        return True

    def start(self):
        if self.in_flight is not None or not self.mail:
            return False
        self.in_flight = self.mail.popleft()
        self.pins += 1
        return True

    def finish(self):
        if self.in_flight is None:
            return False
        ticket, delta = self.in_flight
        require(ticket == self.delivered + 1, "lost or duplicated delivery ticket")
        self.state += delta if self.code == 1 else abs(delta)
        self.delivered += 1
        self.in_flight = None
        self.pins -= 1
        return True

    def attempt(self, compatible=True, restore_ok=True):
        require(not self.frozen, "nested admission freeze")
        self.frozen = True
        old = (self.actor_id, self.epoch, self.code, self.state)
        try:
            if self.pins or self.mail or self.in_flight:
                self.rolled_back += 1
                return False
            if not compatible or not restore_ok:
                self.rolled_back += 1
                return False
            # Snapshot/restore must preserve the owned scalar on success.
            copy = self.state
            next_code = 2 if self.code == 1 else 1
            self.epoch += 1
            self.code = next_code
            self.state = copy
            self.published += 1
            return True
        finally:
            self.frozen = False
            require(self.actor_id == old[0], "actor identity changed")
            if (self.epoch, self.code) == (old[1], old[2]):
                require(self.state == old[3], "rollback changed actor state")

    def verify(self):
        require(self.actor_id == 1 and self.epoch >= 1, "invalid identity or epoch")
        require(self.pins == (1 if self.in_flight is not None else 0), "pin invariant")
        require(self.accepted == self.delivered + len(self.mail) + self.pins, "message conservation")
        require(not self.frozen, "leaked freeze")
        require(self.published + 1 == self.epoch, "epoch drift")

def main():
    transitions = 0
    upgrades = 0
    for seed in range(32):
        rng = random.Random(0xA6700 + seed)
        model = Model()
        for _ in range(3000):
            action = rng.randrange(8)
            if action <= 2:
                model.send(rng.randrange(-4, 5))
            elif action == 3:
                model.start()
            elif action == 4:
                model.finish()
            else:
                if model.attempt(
                    compatible=rng.randrange(6) != 0,
                    restore_ok=rng.randrange(5) != 0):
                    upgrades += 1
            model.verify()
            transitions += 1
        while model.in_flight or model.mail:
            if model.in_flight: model.finish()
            else: model.start()
            model.verify()
        require(model.accepted == model.delivered, "final message lost")
        require(model.pins == 0, "final generation pin leaked")
        require(not model.mail, "final mailbox not drained")
    print(f"PASS: {transitions} modeled interleavings, "
          f"{upgrades} committed upgrades, 32 no-loss/no-dup final drains")

if __name__ == "__main__":
    main()
