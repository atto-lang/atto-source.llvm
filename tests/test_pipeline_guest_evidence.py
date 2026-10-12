#!/usr/bin/env python3
"""Mutation tests for the real compiled three-actor CTest transcript gate."""
import unittest
import json
from contract import verify_three_actor_pipeline_stderr, verify_three_actor_pipeline_trace

VALID = "\n".join([
    "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19",
    "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=1 last=13",
    "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=2 last=14",
    "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=2 last=19",
    "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=3 peak=3 carriers=4",
    "",
])


def native_trace():
    stages = ["source", "transform", "sink", "transform", "source"]
    events = []
    for hop in range(5):
        for op in ["reserve", "publish", "deliver", "finalize"]:
            events.append(dict(seq=len(events), op=op, epoch=1, request="r1",
                               hop=hop, ticket=f"t{hop+1}",
                               stage=stages[hop],
                               variant="Signal.Data" if hop < 3 else "Signal.Result",
                               value=None))
    events.append(dict(seq=len(events), op="complete", epoch=1, request="r1",
                       hop=None, ticket=None, stage=None, variant=None, value=19))
    events.append(dict(seq=len(events), op="stop", epoch=1, request=None,
                       hop=None, ticket=None, stage=None, variant=None, value=None))
    return dict(schemaVersion=1, kind="compiled-actor-pipeline-trace-v1",
                backend="llvm-native",
                sourceGitBlobSha1="46579a917516899612fcd8c65c70b24b995c2c5a",
                events=events)

def trace_line(payload):
    return "SOURCE_PIPELINE_TRACE_JSON " + json.dumps(payload, separators=(",", ":")) + "\n"

class GuestEvidenceTests(unittest.TestCase):
    def test_real_native_trace_layout_and_terminal_barrier(self):
        verify_three_actor_pipeline_trace(trace_line(native_trace()))

    def test_native_trace_rejects_duplicate_late_and_wrong_authority(self):
        mutations = [
            lambda x: x["events"].pop(),
            lambda x: x["events"][2].update(epoch=2),
            lambda x: x["events"][2].update(ticket="t3"),
            lambda x: x["events"][2].update(op="finalize"),
            lambda x: x["events"][4].update(stage="source"),
            lambda x: x["events"][20].update(value=20),
            lambda x: x["events"][21].update(request="r1"),
            lambda x: x["events"][21].update(op="complete"),
            lambda x: x["events"][4].update(seq=3),
        ]
        for mutate in mutations:
            payload=native_trace()
            mutate(payload)
            with self.subTest(mutation=str(mutate)), self.assertRaises(ValueError):
                verify_three_actor_pipeline_trace(trace_line(payload))

    def test_native_trace_rejects_duplicate_record_and_corrupt_json(self):
        payload=native_trace()
        with self.assertRaises(ValueError):
            verify_three_actor_pipeline_trace(trace_line(payload) +
                                              trace_line(payload))
        with self.assertRaises(ValueError):
            verify_three_actor_pipeline_trace("SOURCE_PIPELINE_TRACE_JSON {not-json}\n")
        with self.assertRaises(ValueError):
            verify_three_actor_pipeline_trace(trace_line(payload) * 50)
        with self.assertRaises(ValueError):
            verify_three_actor_pipeline_trace(trace_line(
                dict(payload, schemaVersion=True)))
        duplicate=trace_line(payload).replace(
            '"schemaVersion":1,', '"schemaVersion":1,"schemaVersion":1,', 1)
        with self.assertRaises(ValueError):
            verify_three_actor_pipeline_trace(duplicate)

    def test_complete_compiled_guest_summary(self):
        verify_three_actor_pipeline_stderr(VALID)

    def test_malformed_or_duplicate_summary(self):
        for transcript in [
            VALID.replace("result=19", "result=20"),
            VALID + VALID.splitlines()[0] + "\n",
            VALID.replace("forward=2", "forward=1"),
            VALID.replace("backward=2", "backward=1"),
        ]:
            with self.subTest(transcript=transcript[-65:]), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_three_distinct_finalizers_with_correct_values(self):
        for transcript in [
            VALID.replace("processed=1 last=13", "processed=2 last=13"),
            VALID.replace("processed=2 last=14", "processed=1 last=14"),
            VALID.replace("processed=2 last=19", "processed=2 last=18"),
            VALID.replace("released=1", "released=0"),
            VALID.replace("last=14", "last=19"),
            VALID.replace("slots=3", "slots=4"),
            VALID.replace("active=0", "active=1"),
            VALID.replace("carriers=4", "carriers=0"),
        ]:
            with self.subTest(transcript=transcript[-65:]), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_physical_completion_and_stop_receipts_are_strictly_ordered(self):
        lines = VALID.splitlines()
        mutations = [
            "\n".join([*lines[1:4], lines[0], lines[4]]) + "\n",
            "\n".join([lines[0], lines[4], *lines[1:4]]) + "\n",
            "\n".join([lines[0], lines[2], lines[1], *lines[3:]]) + "\n",
        ]
        for transcript in mutations:
            with self.subTest(transcript=transcript), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_disguised_failure_and_oversized_input_fail_closed(self):
        for transcript in [
            VALID + "logger: SOURCE_PIPELINE_FAILURE code=7\n",
            VALID + "SOURCE_ACTOR_INVARIANT_FAILURE in diagnostic data\n",
            VALID + ("ignored-" * 20000),
            "",
        ]:
            with self.subTest(trailer=transcript[-65:]), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_terminal_failure_is_incompatible_with_success(self):
        for marker in [
            "SOURCE_PIPELINE_FAILURE code=7",
            "SOURCE_PIPELINE_TERMINAL_STATUS code=7",
            "SOURCE_ACTOR_INVARIANT_FAILURE",
        ]:
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(VALID + marker + "\n")

if __name__ == "__main__":
    unittest.main()
