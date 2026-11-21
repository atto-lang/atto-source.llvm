"""Fail-closed source-only test of LLVM's targeted checked-i64 CI admission.

This verifies the *workflow command and test registrations*, not actual
compiler execution or GitHub Actions scheduling.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CI = ROOT / ".github/workflows/ci.yml"
CMAKE = ROOT / "CMakeLists.txt"
NAMES = (
    "numeric_experimental_checked_i64_status_abi",
    "numeric_experimental_checked_pure_function_graph",
    "numeric_experimental_checked_result_function_graph",
)
FILTER = r"^numeric_experimental_checked_(i64_status_abi|pure_function_graph|result_function_graph)$"


class CheckedI64WorkflowAdmissionTest(unittest.TestCase):
    def test_targeted_ctest_must_match_registered_native_compiler_tests(self):
        yaml = CI.read_text(encoding="utf-8")
        step_blocks = re.split(r"(?m)^      - name: ", yaml)
        selected = [part for part in step_blocks if part.startswith(
            "Verify real checked-result compiler ABI before full CTest\n")]
        self.assertEqual(len(selected), 1, "exactly one targeted compiler step")
        step = selected[0]
        self.assertEqual(step.count("\n        run:"), 1,
                         "duplicate run: keys can silently replace the intended command")
        self.assertIn("ctest --test-dir build", step)
        self.assertIn("--no-tests=error", step,
                      "an empty CTest selection is not a compiler test pass")
        self.assertIn("-R '" + FILTER + "'", step,
                      "require all three precise checked-i64 native test names")
        cmake = CMAKE.read_text(encoding="utf-8")
        for name in NAMES:
            with self.subTest(name=name):
                self.assertRegex(cmake, r"add_test\s*\(\s*NAME\s+" +
                                 re.escape(name) + r"\b")

    def test_ctest_manifest_gate_is_a_separate_named_step(self):
        yaml = CI.read_text(encoding="utf-8")
        steps = re.split(r"(?m)^      - name: ", yaml)
        matching = [s for s in steps if s.startswith(
            "Verify checked-i64 CTest inventory before full native suite\n")]
        self.assertEqual(len(matching), 1)
        self.assertEqual(matching[0].count("\n        run:"), 1)
        self.assertIn("ctest_manifest_gate.py --build-dir build --min-tests 250",
                      matching[0])


    def test_checked_i64_release_inventory_is_mandatory_and_precedes_execution(self):
        yaml = CI.read_text(encoding="utf-8")
        manifest = (ROOT / "tests/ctest_manifest_gate.py").read_text(encoding="utf-8")
        inventory = yaml.index("      - name: Verify checked-i64 CTest inventory before full native suite")
        targeted = yaml.index("      - name: Verify real checked-result compiler ABI before full CTest")
        self.assertLess(inventory, targeted, "inventory gate must fail before guest execution")
        for name, harness in (
            ("numeric_experimental_checked_i64_status_abi", "native_checked_scalar_experimental.py"),
            ("numeric_experimental_checked_pure_function_graph", "native_checked_pure_functions.py"),
            ("numeric_experimental_checked_result_function_graph", "native_checked_result_functions.py"),
        ):
            with self.subTest(name=name):
                self.assertIn('"' + name + '": "' + harness + '"', manifest)
                self.assertIn("CHECKED_I64_GUEST_TESTS", manifest)
        self.assertIn("critical CTest timeout is unsafe", manifest)

    def test_native_evidence_upload_is_short_lived_and_not_self_certification(self):
        yaml = CI.read_text(encoding="utf-8")
        self.assertIn("--output-junit", yaml)
        self.assertIn("build/checked-i64-ctest.xml", yaml)
        self.assertIn("build/full-ctest.xml", yaml)
        self.assertIn('"verified_compiled_claim": False', yaml)
        self.assertIn("if: ${{ always() }}", yaml)
        self.assertIn("retention-days: 3", yaml)
        self.assertIn(
            "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02", yaml)
        self.assertNotIn("private-oreslang-source", yaml)

    def test_full_native_ci_steps_are_unique(self):
        yaml = CI.read_text(encoding="utf-8")
        names = re.findall(r"(?m)^      - name: ([^\r\n]+)$", yaml)
        self.assertTrue(names)
        self.assertEqual(len(names), len(set(names)),
                         "duplicate step names hide repeated native CI phases")
        native_steps = (
            "Verify real checked-result compiler ABI before full CTest",
            "Verify checked-i64 CTest inventory before full native suite",
            "Verify parser, IR and native execution",
            "Repeat actor stress and cancellation races",
            "ASan and UBSan native actor memory and concurrency smoke",
            "Sanitized actor-owned generational graph heap",
            "ThreadSanitizer actor data-race stress",
        )
        for step in native_steps:
            with self.subTest(step=step):
                self.assertEqual(names.count(step), 1)

    def test_duplicate_native_phase_mutation_would_be_denied(self):
        yaml = CI.read_text(encoding="utf-8")
        names = re.findall(r"(?m)^      - name: ([^\r\n]+)$", yaml)
        self.assertEqual(len(names), len(set(names)))
        duplicated = yaml + "\n      - name: Verify parser, IR and native execution\n"
        mutated_names = re.findall(r"(?m)^      - name: ([^\r\n]+)$", duplicated)
        self.assertNotEqual(len(mutated_names), len(set(mutated_names)),
                            "duplicate native CTest phase must break the uniqueness invariant")



if __name__ == "__main__":
    unittest.main()
