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


if __name__ == "__main__":
    unittest.main()
