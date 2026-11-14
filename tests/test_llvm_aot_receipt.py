"""AOT receipt audit: source and evidence hygiene, not HTTP3 wire testing."""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "contracts/native-llvm-aot-v1.json"


def blob(raw: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()


class NativeAotReceiptTest(unittest.TestCase):
    def setUp(self):
        self.r = json.loads(DOC.read_text())
        self.h = json.loads((ROOT / "contracts/native-llvm-aot-hardening-v1.json").read_text())

    def test_pinned_frontend_and_emitter_match_source(self):
        s = self.r["source_snapshot"]
        self.assertEqual(s["files"], 30)
        self.assertEqual(s["full_tree"], "d02b401e5c7a3a524c4a70db8db8520c6ae408ee")
        # Original scalar AOT receipt is historical and immutable; a newer
        # independent audit pins today's output-safety implementation.
        self.assertEqual(s["direct_llvm_codegen_blob"],
                         "0b0ff561ada2e2abb722c894eec02633d7ecbf91")
        self.assertEqual(s["native_test_script_blob"],
                         "fdfc0a303403dd82040a5a071ab14b89d565ad74")
        self.assertEqual(blob((ROOT / "src/main.rs").read_bytes()),
                         self.h["llvm_emitter_blob"])
        self.assertEqual(blob((ROOT / "ci/test_native_aot.sh").read_bytes()),
                         self.h["native_ci_script_blob"])
        self.assertEqual(blob((ROOT / "frontend/src/parser.rs").read_bytes()),
                         s["frontend_parser_blob"])
        self.assertEqual(blob((ROOT / "frontend/src/check.rs").read_bytes()),
                         s["frontend_check_blob"])

    def test_hardened_compiler_output_contract_and_scope(self):
        h = self.h
        self.assertEqual(h["status"],
                         "PASS_LINUX_X86_64_SCALAR_GUEST_AND_OUTPUT_HARDENING")
        self.assertEqual(h["base_receipt"], "contracts/native-llvm-aot-v1.json")
        self.assertEqual(blob(DOC.read_bytes()), h["base_receipt_blob"])
        self.assertEqual(h["audited_source_commit"],
                         "ec2c04024b10f0dc1005c90fa73c6e741615df39")
        self.assertEqual(h["audited_full_tree"],
                         "bf71ea154bf87bb80c7534678e716d5daa94f455")
        self.assertEqual(h["audited_blob_count"], 32)
        self.assertIn("/actions/runs/43651073559", h["runner"]["run"])
        self.assertEqual(h["runner"]["job_conclusion"], "success")
        self.assertTrue(h["runner"]["sha_exact_local_mirror"])
        self.assertFalse(h["runner"]["cross_org_checkout_credentials"])
        verification = h["verification"]
        self.assertEqual(verification["total_native_atto_cases"], 1255)
        self.assertEqual(verification["frontend_unit_tests_passed"], 4)
        self.assertEqual(verification["actual_exit_code"], 42)
        for name, result in verification.items():
            if name.endswith("_rejected") or name.endswith("_preserved_after_source_or_clang_failure") \
               or name.endswith("_removed") or name.endswith("_passed") \
               or name.endswith("_true") or name.endswith("_verified") \
               or name in ("native_llvm_to_elf_guest_execution",
                           "output_uses_exclusive_staging_file",
                           "output_commits_only_after_full_success",
                           "source_output_alias_rejected",
                           "symlink_to_input_rejected",
                           "hardlink_to_input_rejected",
                           "prior_output_preserved_after_source_or_clang_failure",
                           "staged_temporary_artifacts_removed",
                           "deterministic_llvm_ir",
                           "guest_vm_required",
                           "guest_gc_required_for_scalar_subset"):
                if name in ("guest_vm_required", "guest_gc_required_for_scalar_subset"):
                    self.assertFalse(result, name)
                else:
                    self.assertTrue(result, name)
        for field, value in h["limitations"].items():
            if isinstance(value, bool):
                self.assertFalse(value, field)
        self.assertEqual(sum(x["cases"] for x in h["generated_artifacts"]), 1255)
        self.assertEqual(
            [x["elf_sha256"] for x in h["generated_artifacts"]],
            [x["elf_sha256"] for x in self.r["fixtures"]]
        )

    def test_1255_native_atto_fixture_sources_are_exact(self):
        total = 0
        for row in self.r["fixtures"]:
            source = ROOT / row["source"]
            self.assertTrue(source.is_file())
            self.assertEqual(source.suffix, ".atto")
            raw = source.read_bytes()
            self.assertEqual(blob(raw), row["blob_sha"])
            self.assertTrue(raw.startswith(b"// expect-exit: 42\n"))
            main = raw.rsplit(b"pub fnc main(): int {", 1)[1]
            cases = sum(line.startswith(b"  if ") for line in main.splitlines())
            self.assertEqual(cases, row["cases"])
            for field in ("ir_sha256", "elf_sha256"):
                self.assertEqual(len(row[field]), 64)
                int(row[field], 16)
            total += cases
        self.assertEqual(total, 1255)
        self.assertEqual(total, self.r["execution"]["total_cases"])

    def test_execution_is_actual_native_elf_and_strictly_scalar(self):
        run = self.r["runner"]
        execution = self.r["execution"]
        self.assertIn("/actions/runs/43649930185", run["run"])
        self.assertTrue(run["exact_git_tree_match"])
        self.assertTrue(run["private_no_cross_org_checkout_token"])
        self.assertEqual(run["toolchain_clang"], "Ubuntu clang 18.1.3")
        self.assertEqual(execution["executable_format"], "ELF64")
        self.assertTrue(execution["actual_guest_execute"])
        self.assertTrue(execution["direct_cli_native_elf_verified"])
        self.assertTrue(execution["checked_i64_add_sub_mul_neg_trap_passed"])
        self.assertTrue(execution["asan_ubsan_smoke_passed"])
        self.assertTrue(execution["four_compiler_unit_tests_passed"])
        self.assertTrue(execution["deterministic_llvm_ir"])
        self.assertTrue(execution["invalid_guest_E_UNBOUND_rejected"])
        self.assertTrue(execution["unsupported_io_rejected"])
        for field in ("runtime_guest_vm", "runtime_wasm_interpreter",
                      "runtime_jit", "guest_heap", "guest_gc"):
            self.assertFalse(execution[field])
        self.assertEqual(execution["scope"], "SCALAR_PURE_FUNCTIONS_ONLY")
        self.assertEqual(execution["expected_native_process_exit_status"], 42)
        for field, value in self.r["unsupported"].items():
            self.assertFalse(value, field)


if __name__ == "__main__":
    unittest.main()
