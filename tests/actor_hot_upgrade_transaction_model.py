#!/usr/bin/env python3
"""Source-free, deterministic actor hot-upgrade interleaving oracle.

This is a state-machine MODEL, not runtime source tests or compiler execution.
It checks admitted-message conservation and same-schema rollback semantics.
"""
import collections
import dataclasses
import random

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
        self.state += delta if self.code == 1 else abs(delta)
        self.delivered += 1
        self.in_flight = None
        self.pins -= 1
        return True

    def attempt(self, compatible=True, restore_ok=True):
        assert not self.frozen
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
            assert self.actor_id == old[0]
            if (self.epoch, self.code) == (old[1], old[2]):
                assert self.state == old[3]

    def verify(self):
        assert self.actor_id == 1 and self.epoch >= 1
        assert self.pins == (1 if self.in_flight is not None else 0)
        assert self.accepted == self.delivered + len(self.mail) + self.pins
        assert not self.frozen
        assert self.published + 1 == self.epoch

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
        assert model.accepted == model.delivered
        assert model.pins == 0
        assert not model.mail
    print(f"PASS: {transitions} modeled interleavings, "
          f"{upgrades} committed upgrades, 32 no-loss/no-dup final drains")

if __name__ == "__main__":
    main()
