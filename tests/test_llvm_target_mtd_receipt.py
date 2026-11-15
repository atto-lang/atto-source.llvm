"""Executed target actor LLVM lowering receipts: exact historical SHA, no wire claims."""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECEIPT = ROOT / "contracts/native-llvm-target-mtd-v1.json"


def git_blob(raw: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()


class TargetNativeEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.e = json.loads(RECEIPT.read_bytes())

    def test_exact_native_linker_and_typed_frontend_blobs(self):
        e = self.e
        self.assertEqual(e["schema"], "atto.llvm.target-mtd-critical-aot.v1")
        self.assertEqual(e["status"],
                         "PASS_BOUND_NATIVE_RECEIVERS_AND_SEALED_SCALAR_CRITICAL_CALLBACKS")
        self.assertEqual(e["source"]["exact_git_tree"],
                         "a6d2f2ab5155fac02d29fec426c2014c30ad4a98")
        self.assertEqual(e["source"]["blob_count"], 63)
        for path, name in (
            ("src/main.rs", "codegen_blob"),
            ("src/target_native.rs", "target_ast_linker_blob"),
            ("frontend/src/target_syntax.rs", "target_header_grammar_blob"),
            ("frontend/src/check.rs", "checked_production_typechecker_blob"),
            ("runtime/actor_runtime.c", "native_actor_runtime_blob"),
            ("ci/test_native_actors.sh", "native_guest_test_script_blob"),
        ):
            self.assertEqual(git_blob((ROOT / path).read_bytes()),
                             e["source"][name],path)

    def test_real_guest_runs_and_fail_closed_diagnostics(self):
        e = self.e
        self.assertIn("/actions/runs/43666674489",e["runner"]["run"])
        self.assertEqual(e["runner"]["conclusion"],"success")
        self.assertTrue(e["runner"]["full_tree_match"])
        self.assertTrue(e["runner"]["no_cross_org_checkout_secret"])
        v = e["verified"]
        self.assertEqual(v["compiled_native_actor_fixtures"],6)
        self.assertEqual(v["executions_per_fixture"],20)
        self.assertEqual(v["total_native_atto_actor_executions"],120)
        self.assertEqual(v["negative_actor_compilation_fixtures"],15)
        self.assertEqual(v["abi_total_admitted_messages"],8000)
        self.assertEqual(v["previous_native_scalar_protocol_checks"],1255)
        self.assertEqual(len(e["positive_guest_blobs"]),2)
        self.assertEqual(len(e["negative_guest_blobs"]),12)
        self.assertEqual(sum(row["executions"] for row in e["positive_guest_blobs"]),40)
        for row in e["positive_guest_blobs"] + e["negative_guest_blobs"]:
            self.assertEqual(git_blob((ROOT / row["path"]).read_bytes()),
                             row["git_blob"],row["path"])
        for row in e["negative_guest_blobs"]:
            self.assertIn(row["diagnostic"],
                          (ROOT / row["path"]).read_text())

    def test_critical_state_scope_is_bounded(self):
        x = self.e["executable_scope"]
        self.assertEqual(x["guest_source"],".atto")
        self.assertTrue(x["actual_native_machine_code"])
        self.assertTrue(x["no_wasm_or_jit_guest"])
        self.assertTrue(x["no_guest_gc_in_bounded_profile"])
        self.assertEqual(x["guard_read"],"one scalar state field, no effects")
        self.assertFalse(x["concurrent_reader_writer_locks"])
        self.assertFalse(x["full_borrow_checker"])
        for k, v in self.e["unsupported"].items():
            self.assertFalse(v,k)
        for k in (
            "two_actor_types_same_mtd_name_pass",
            "independent_actor_guarded_state_pass",
            "direct_state_access_rejected",
            "critical_read_mutation_rejected",
            "critical_write_requires_mut_self",
            "binder_escape_rejected",
            "await_in_critical_rejected",
            "nested_critical_rejected",
            "effectful_critical_rejected",
            "unrelocated_nominal_type_rejected",
            "actor_sanitizers_asan_ubsan_passed",
            "output_atomicity_tests_passed",
        ):
            self.assertTrue(self.e["verified"][k], k)


if __name__ == "__main__":
    unittest.main()
