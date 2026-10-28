#!/usr/bin/env python3
"""Source-free hostile transcript regressions; not compiled .ores execution."""
import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).with_name("source_actor_e2e.py")
spec = importlib.util.spec_from_file_location("source_actor_e2e", path)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def receipt(n, result):
    return {
        "constructor_on_worker": 1, "ready": 1, "released": 1,
        "accepted": n, "processed": n, "abandoned": 0, "last": result,
    }

def observed():
    lines = []
    for slot, actor, values in (
        (0, 17, [(1, 2), (2, 8), (3, 22), (4, 52)]),
        (1, 19, [(5, 10), (6, 32), (7, 78), (8, 172)]),
    ):
        for seq, (payload, value) in enumerate(values, 1):
            lines.append(f"SOURCE_ACTOR_GUEST_HANDLER actor={actor} slot={slot} "
                         f"seq={seq} payload={payload} result={value}")
    lines.append("SOURCE_ACTOR_SPAWN_STOP_VERIFIED "
                 "constructor_on_worker=1 released=1 ready=1 "
                 "accepted=4 processed=4 abandoned=0 last=52")
    lines.append("SOURCE_ACTOR_GUEST_HANDLER actor=19 slot=1 "
                 "seq=5 payload=9 result=362")
    lines.append("SOURCE_ACTOR_SPAWN_STOP_VERIFIED "
                 "constructor_on_worker=1 released=1 ready=1 "
                 "accepted=5 processed=5 abandoned=0 last=362")
    return [receipt(4, 52), receipt(5, 362)], lines

class GuestProgressProofTests(unittest.TestCase):
    def test_positive_candidate_transcript_is_still_not_abi_proof(self):
        rows, lines = observed()
        self.assertEqual({0: 17, 1: 19},
                         module.verify_two_actor_guest_progress(rows, lines))

    def deny(self, mutate):
        rows, lines = observed()
        mutate(rows, lines)
        with self.assertRaises((AssertionError, StopIteration)):
            module.verify_two_actor_guest_progress(rows, lines)

    def test_zero_executed_messages(self):
        self.deny(lambda r, _: r[0].update(processed=0, abandoned=4, last=0))

    def test_partial_execution(self):
        self.deny(lambda r, _: r[1].update(processed=4, abandoned=1, last=172))

    def test_wrong_last_result(self):
        self.deny(lambda r, _: r[1].update(last=361))

    def test_missing_guest_handler(self):
        self.deny(lambda _, l: l.pop(0))

    def test_repeated_guest_result(self):
        self.deny(lambda _, l: l.__setitem__(1, l[0]))

    def test_foreign_actor_identity_collides(self):
        self.deny(lambda _, l: l.__setitem__(4, l[4].replace("actor=19", "actor=17")))

    def test_false_guest_result(self):
        self.deny(lambda _, l: l.__setitem__(9, l[9].replace("result=362", "result=363")))

    def test_b_result_before_a_retirement(self):
        self.deny(lambda _, l: (l.insert(0, l.pop(9))))

    def test_b_never_physically_stops(self):
        self.deny(lambda _, l: l.pop())

    def test_duplicate_field_in_guest_trace(self):
        self.deny(lambda _, l: l.__setitem__(0, l[0] + " result=2"))

    def test_fake_extra_receipt(self):
        self.deny(lambda _, l: l.append(l[-1]))

    def test_same_actor_slot_for_b(self):
        self.deny(lambda _, l: l.__setitem__(
            9, l[9].replace("slot=1", "slot=0")))

if __name__ == "__main__":
    unittest.main()
