#!/usr/bin/env python3
"""Source-free hostile actor transcript regressions; NOT compiled guest execution."""
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
    for slot, actor, heap, values in (
        (0, 17, 71, [(1, 2), (2, 8), (3, 22), (4, 52)]),
        (1, 19, 73, [(5, 10), (6, 32), (7, 78), (8, 172)]),
    ):
        for seq, (payload, value) in enumerate(values, 1):
            lines.append(f"SOURCE_ACTOR_GUEST_HANDLER actor={actor} heap={heap} "
                         f"slot={slot} seq={seq} payload={payload} result={value}")
    lines.append("SOURCE_ACTOR_HEAP_FIELDS actor=17 heap=71 slot=0 "
                 "init=1 reads=4 writes=4 dropped=0")
    lines.append("SOURCE_ACTOR_SPAWN_STOP_VERIFIED "
                 "constructor_on_worker=1 released=1 ready=1 "
                 "accepted=4 processed=4 abandoned=0 last=52")
    lines.append("SOURCE_ACTOR_GUEST_HANDLER actor=19 heap=73 slot=1 "
                 "seq=5 payload=9 result=362")
    lines.append("SOURCE_ACTOR_HEAP_FIELDS actor=19 heap=73 slot=1 "
                 "init=1 reads=5 writes=5 dropped=0")
    lines.append("SOURCE_ACTOR_SPAWN_STOP_VERIFIED "
                 "constructor_on_worker=1 released=1 ready=1 "
                 "accepted=5 processed=5 abandoned=0 last=362")
    return [receipt(4, 52), receipt(5, 362)], lines


def replace_line(lines, prefix, original, replacement):
    matches = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    assert len(matches) == 1, matches
    i = matches[0]
    assert original in lines[i], lines[i]
    lines[i] = lines[i].replace(original, replacement)


class GuestProgressProofTests(unittest.TestCase):
    def test_positive_candidate_transcript_is_not_guest_proof(self):
        rows, lines = observed()
        self.assertEqual({0: 17, 1: 19},
                         module.verify_two_actor_guest_progress(rows, lines))

    def deny(self, mutate):
        rows, lines = observed()
        mutate(rows, lines)
        with self.assertRaises((AssertionError, StopIteration)):
            module.verify_two_actor_guest_progress(rows, lines)

    def test_zero_processed(self):
        self.deny(lambda r, _: r[0].update(processed=0, abandoned=4, last=0))

    def test_partial_processed(self):
        self.deny(lambda r, _: r[1].update(processed=4, abandoned=1, last=172))

    def test_wrong_last(self):
        self.deny(lambda r, _: r[1].update(last=361))

    def test_missing_guest_handler(self):
        self.deny(lambda _, l: l.pop(0))

    def test_repeated_guest_handler(self):
        self.deny(lambda _, l: l.__setitem__(1, l[0]))

    def test_same_actor_identity(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_GUEST_HANDLER actor=19 heap=73 slot=1 seq=1 ",
            "actor=19", "actor=17"))

    def test_false_guest_result(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_GUEST_HANDLER actor=19 heap=73 slot=1 seq=5 ",
            "result=362", "result=363"))

    def test_b_last_before_a_retired(self):
        self.deny(lambda _, l: l.insert(0, l.pop(next(
            i for i, row in enumerate(l) if "seq=5 payload=9" in row))))

    def test_missing_b_stop(self):
        self.deny(lambda _, l: l.pop())

    def test_duplicate_handler_field(self):
        self.deny(lambda _, l: l.__setitem__(0, l[0] + " result=2"))

    def test_extra_stop(self):
        self.deny(lambda _, l: l.append(l[-1]))

    def test_slot_collision(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_GUEST_HANDLER actor=19 heap=73 slot=1 seq=5 ",
            "slot=1", "slot=0"))

    def test_same_heap_for_both_actors(self):
        self.deny(lambda _, l: l.__setitem__(
            slice(None), [row.replace("heap=73", "heap=71") for row in l]))

    def test_heap_switch_during_a(self):
        self.deny(lambda _, l: l.__setitem__(
            2, l[2].replace("heap=71", "heap=79")))

    def test_heap_identity_missing(self):
        self.deny(lambda _, l: l.__setitem__(
            0, l[0].replace("heap=71 ", "")))

    def test_zero_heap_domain(self):
        self.deny(lambda _, l: l.__setitem__(
            0, l[0].replace("heap=71", "heap=0")))

    def test_missing_field_access_receipt(self):
        self.deny(lambda _, l: l.pop(next(i for i, row in enumerate(l)
                                        if row.startswith("SOURCE_ACTOR_HEAP_FIELDS"))))

    def test_field_reads_missing(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_HEAP_FIELDS actor=17 ", "reads=4", "reads=0"))

    def test_field_writes_missing(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_HEAP_FIELDS actor=19 ", "writes=5", "writes=4"))

    def test_field_owner_mismatch(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_HEAP_FIELDS actor=19 ", "actor=19", "actor=17"))

    def test_field_heap_mismatch(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_HEAP_FIELDS actor=17 ", "heap=71", "heap=77"))

    def test_field_trace_overflow(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_HEAP_FIELDS actor=17 ", "dropped=0", "dropped=1"))

    def test_field_receipt_before_guest_returns(self):
        self.deny(lambda _, l: l.insert(0, l.pop(next(
            i for i, row in enumerate(l)
            if row.startswith("SOURCE_ACTOR_HEAP_FIELDS actor=17 ")))))

    def test_extra_field_receipt(self):
        self.deny(lambda _, l: l.append(next(
            row for row in l if row.startswith("SOURCE_ACTOR_HEAP_FIELDS"))))

    def test_duplicate_field_key(self):
        self.deny(lambda _, l: replace_line(
            l, "SOURCE_ACTOR_HEAP_FIELDS actor=17 ", "reads=4", "reads=4 reads=4"))


if __name__ == "__main__":
    unittest.main()
