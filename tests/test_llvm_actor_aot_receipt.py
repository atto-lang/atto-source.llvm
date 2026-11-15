"""Exact source/guest proof for bounded LLVM native actors, not full actor model."""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECEIPT = ROOT / "contracts/native-llvm-actor-aot-v1.json"


def blob(content: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest()


class ActorAotEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.m = json.loads(RECEIPT.read_text())

    def test_native_actor_llvm_and_c_runtime_match_pinned_blobs(self):
        m = self.m
        self.assertEqual(m["schema"], "atto.llvm.native-actor-bootstrap.v1")
        self.assertEqual(m["status"], "PASS_LINUX_X86_64_BOUNDED_LEGACY_ACTOR_AOT")
        src = m["source"]
        self.assertEqual(src["full_git_tree"], "f40cb5044af7ffb55cbd05dd7024cc7963d6042d")
        self.assertEqual(src["blob_count"], 43)
        for path, key in (("src/main.rs", "codegen_blob"),
                          ("runtime/actor_runtime.c", "native_runtime_blob"),
                          ("ci/test_native_actors.sh", "test_script_blob"),
                          ("frontend/src/check.rs", "frontend_typechecker_blob")):
            self.assertEqual(blob((ROOT / path).read_bytes()), src[key], path)
        # The original stress proof remains an immutable 43-blob snapshot.
        # A newer source harness may test more capacity bounds; do not rewrite
        # the historical Git blob or claim it ran in the earlier execution.
        self.assertEqual(src["abi_stress_blob"],
                         "3258ca2c5fe0a7f08dc07697e58585e099afc074")
        self.assertIn("/actions/runs/43657189906", m["carrier"]["run"])
        self.assertEqual(m["carrier"]["conclusion"], "success")
        self.assertTrue(m["carrier"]["exact_tree_verified"])
        self.assertTrue(m["carrier"]["source_blob_verification"])
        self.assertFalse(m["carrier"]["cross_org_checkout_secret"])

    def test_executed_native_actor_fixture_sources(self):
        m = self.m
        self.assertEqual(m["executed_tests"]["native_actor_guest_fixtures"], 4)
        self.assertEqual(m["executed_tests"]["total_native_actor_guest_executions"], 80)
        self.assertEqual(m["executed_tests"]["negative_compilation_rejections"], 3)
        self.assertEqual(len(m["guest_fixture_blobs"]), 4)
        self.assertEqual(len(m["negative_fixture_blobs"]), 3)
        for row in m["guest_fixture_blobs"]:
            self.assertTrue(row["path"].startswith("tests/actors/pass/"))
            self.assertEqual(blob((ROOT / row["path"]).read_bytes()), row["blob"])
            self.assertEqual(row["executions"], 20)
        for row in m["negative_fixture_blobs"]:
            self.assertTrue(row["path"].startswith("tests/actors/fail/"))
            self.assertEqual(blob((ROOT / row["path"]).read_bytes()), row["blob"])

    def test_native_memory_mailbox_and_sanitizer_bounds(self):
        p = self.m["runtime_profile"]
        self.assertEqual(p["guest_source_extension"], ".atto")
        self.assertEqual(p["actor_model"], "bounded pthread-per-actor bootstrap")
        self.assertEqual(p["actor_own_stack_bytes"], 262144)
        self.assertEqual(p["actor_own_arena_bytes"], 65536)
        self.assertEqual(p["mailbox_capacity_per_actor"], 128)
        self.assertEqual(p["max_actor_instances"], 32)
        self.assertEqual(p["relative_arena_offsets_bits"], 32)
        self.assertEqual(p["max_global_state_slots"], 1024)
        self.assertTrue(p["native_actor_handler_execution"])
        self.assertTrue(p["no_guest_gc_in_profile"])
        self.assertTrue(p["no_guest_interpreter"])
        e = self.m["executed_tests"]
        self.assertEqual(e["abi_concurrent_producers"], 4)
        self.assertEqual(e["abi_actor_instances"], 8)
        self.assertEqual(e["abi_messages_per_run"], 1600)
        self.assertEqual(e["abi_runs"], 5)
        self.assertEqual(e["abi_total_messages"], 8000)
        self.assertEqual(e["previous_native_scalar_protocol_cases_retained"], 1255)
        self.assertTrue(e["abi_stale_or_forged_generation_rejected"])
        self.assertTrue(e["abi_drain_on_close"])
        self.assertTrue(e["asan_ubsan_actor_guest"])
        self.assertTrue(e["asan_ubsan_abi_stress"])
        self.assertEqual(e["mailbox_full_retry_path"],
                         "IMPLEMENTED_BUT_NOT_EXPLICITLY_ASSERTED_NONZERO")

    def test_missing_actor_model_features_not_overclaimed(self):
        p = self.m["runtime_profile"]
        self.assertEqual(p["send_full_result"], "NATIVE_TRAP_NOT_YET_RESULT")
        self.assertEqual(p["actor_spawn_in_actor"], "NOT_SUPPORTED")
        for feature, claim in self.m["unsupported"].items():
            self.assertFalse(claim, feature)
        self.assertEqual(
            self.m["unsupported"]["syntax_namespace_declare_actor_mtd_native"], False
        )


if __name__ == "__main__":
    unittest.main()
