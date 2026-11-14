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

    def test_pinned_frontend_and_emitter_match_source(self):
        s = self.r["source_snapshot"]
        self.assertEqual(s["files"], 30)
        self.assertEqual(s["full_tree"], "d02b401e5c7a3a524c4a70db8db8520c6ae408ee")
        self.assertEqual(blob((ROOT / "src/main.rs").read_bytes()),
                         s["direct_llvm_codegen_blob"])
        self.assertEqual(blob((ROOT / "ci/test_native_aot.sh").read_bytes()),
                         s["native_test_script_blob"])
        self.assertEqual(blob((ROOT / "frontend/src/parser.rs").read_bytes()),
                         s["frontend_parser_blob"])
        self.assertEqual(blob((ROOT / "frontend/src/check.rs").read_bytes()),
                         s["frontend_check_blob"])

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
