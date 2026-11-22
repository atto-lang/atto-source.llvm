"""Evidence integrity for native LLVM nominal-index and sealed-state hardening.

Historical evidence is immutable; this receipt identifies the newer exact
source tree and executed private carrier. No full actor/HTTP3 parity inference.
"""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECEIPT = ROOT / "contracts/native-llvm-nominal-sealed-hardening-v1.json"


def blob(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


class NominalSealedAudit(unittest.TestCase):
    def setUp(self):
        self.p = json.loads(RECEIPT.read_text())

    def test_exact_source_sha_and_runner_provenance(self):
        p = self.p
        self.assertEqual(p["schema"], "atto.llvm.nominal-sealed-hardening.v1")
        self.assertEqual(p["status"],
                         "PASS_NATIVE_ACTOR_NOMINAL_INDEX_AND_SEALED_FIELD_AUDIT")
        s = p["source"]
        self.assertEqual(s["verified_commit"],
                         "b80020c46d1a496b663243e1a4a942edcfb208f6")
        self.assertEqual(s["exact_git_tree"],
                         "c5e1f150fa628f3985a535efd83bcfcbffb03cb0")
        self.assertEqual(s["exact_blob_count"], 69)
        for path, field in (
            ("frontend/src/check.rs", "checked_type_resolver_blob"),
            ("src/target_native.rs", "native_target_linker_blob"),
            ("ci/test_native_actors.sh", "native_actor_script_blob"),
            ("ci/test_native_aot.sh", "scalar_script_blob"),
        ):
            self.assertEqual(blob((ROOT / path).read_bytes()), s[field], path)
        carrier = p["carrier"]
        self.assertEqual(carrier["conclusion"], "success")
        self.assertTrue(carrier["exact_full_tree_verified"])
        self.assertTrue(carrier["checkout_uses_local_carrier_token_only"])
        self.assertFalse(carrier["cross_org_checkout_secrets"])
        self.assertIn("/actions/runs/43669072054", carrier["run"])

    def test_adversarial_cases_and_exact_diagnostics(self):
        p = self.p
        self.assertEqual(len(p["adversarial_fixtures"]), 4)
        cases = p["adversarial_fixtures"]
        self.assertEqual([c["expected_error"] for c in cases],
                         ["E_SEALED", "E_TYPE_INDEX", "E_TYPE_INDEX", "E_TYPE_INDEX"])
        for row in cases:
            path = ROOT / row["path"]
            self.assertEqual(blob(path.read_bytes()), row["git_blob"])
            self.assertIn("// expect-reject: " + row["expected_error"],
                          path.read_text())
        fixes = p["audit_fixes"]
        self.assertEqual(fixes["unresolved_type_index_checked"], "E_TYPE_INDEX")
        self.assertEqual(fixes["forbidden_public_actor_state"], "E_SEALED")
        self.assertTrue(fixes["reparsed_receiver_abi_types_revalidated"])
        self.assertTrue(fixes["historical_scalar_and_actor_receipts_preserved_unchanged"])

    def test_executed_native_scope_without_unverified_claims(self):
        e = self.p["executed"]
        self.assertTrue(e["real_atto_guest_native_elf"])
        self.assertEqual(e["native_guest_actor_fixture_count"], 6)
        self.assertEqual(e["native_guest_actor_runs_per_fixture"], 20)
        self.assertEqual(e["native_guest_actor_executions"], 120)
        self.assertEqual(e["fail_closed_actor_compilation_cases"], 19)
        self.assertEqual(e["adversarial_new_cases"], 4)
        self.assertEqual(e["total_admitted_mailbox_messages"], 8000)
        self.assertEqual(e["native_quic_http3_scalar_checks"], 1255)
        self.assertTrue(e["asan_ubsan_native_actor_guest_pass"])
        self.assertTrue(e["asan_ubsan_host_mailbox_stress_pass"])
        for key, enabled in self.p["scope"].items():
            if key in ("native_legacy_and_target_scalar_actor_profile_only",
                       "actor_owned_private_arenas_and_dedicated_stacks",
                       "bounded_critical_read_one_scalar_field",
                       "bounded_critical_write_one_pure_scalar_assignment",
                       "no_guest_gc_in_this_bounded_native_profile"):
                self.assertTrue(enabled, key)
            else:
                self.assertFalse(enabled, key)


if __name__ == "__main__":
    unittest.main()
