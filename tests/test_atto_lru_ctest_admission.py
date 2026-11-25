"""Source-free adversarial checks for native LLVM LRU CTest JSON admission."""
from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import json
import sys
import tempfile
import unittest

from atto_lru_ctest_admission import (
    REQUIRED, RUNNER_TESTS, SOURCE_TEST, no_duplicate_pairs, validate
)
from atto_lru_llvm_source import PREFIX


class CTestInventoryAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="atto-ctest-admission-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.build = self.root / "build/atto-lru"
        self.build.mkdir(parents=True)
        (self.root / "tests/fixtures").mkdir(parents=True)
        (self.root / PREFIX).mkdir(parents=True)
        (self.root / "tests/contract.py").touch()
        (self.root / "tests/atto_lru_llvm_source.py").touch()
        (self.build / "oreslang-llvmc").touch()
        runtime = self.root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
        runtime.parent.mkdir(parents=True)
        runtime.touch()
        self.clang = self.root / "clang"
        self.clang.touch()
        self.tests = []
        for name, (case, expected_exit, needs_array) in RUNNER_TESTS.items():
            fixture = self.root / "tests/fixtures/../../" / PREFIX / (case + ".atto")
            (self.root / PREFIX / (case + ".atto")).touch()
            command = [
                sys.executable, str(self.root / "tests/contract.py"),
                "--compiler", str(self.build / "oreslang-llvmc"),
                "--fixture", str(fixture), "--run", str(expected_exit),
                "--clang", str(self.clang),
            ]
            if needs_array:
                command.extend(["--native-runtime", str(runtime)])
            self.tests.append({
                "name": name,
                "command": command,
                "properties": [
                    {"name": "WORKING_DIRECTORY", "value": str(self.build)}
                ],
            })
        self.tests.append({
            "name": SOURCE_TEST,
            "command": [
                sys.executable, str(self.root / "tests/atto_lru_llvm_source.py")
            ],
            "properties": [
                {"name": "TIMEOUT", "value": 20},
                {"name": "WORKING_DIRECTORY", "value": str(self.build)}
            ],
        })
        self.assertEqual({t["name"] for t in self.tests}, REQUIRED)

    def check(self, data=None):
        validate({"tests": self.tests if data is None else data},
                 self.root, self.build, self.clang)

    def fail_for(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.check()

    def test_all_five_real_commands_admitted(self):
        self.check()

    def test_will_fail_inverts_exit_and_is_denied(self):
        self.tests[0]["properties"].append({"name": "WILL_FAIL", "value": True})
        self.fail_for("inverted")

    def test_even_false_will_fail_override_is_denied(self):
        self.tests[0]["properties"].append({"name": "WILL_FAIL", "value": False})
        self.fail_for("inverted")

    def test_disabled_test_denied(self):
        self.tests[0]["properties"].append({"name": "DISABLED", "value": True})
        self.fail_for("inverted")

    def test_skip_return_code_denied(self):
        self.tests[0]["properties"].append({"name": "SKIP_RETURN_CODE", "value": 42})
        self.fail_for("inverted")

    def test_skip_regex_denied(self):
        self.tests[0]["properties"].append({"name": "SKIP_REGULAR_EXPRESSION", "value": ["skip"]})
        self.fail_for("inverted")

    def test_pass_regex_override_denied(self):
        self.tests[0]["properties"].append({"name": "PASS_REGULAR_EXPRESSION", "value": ["ok"]})
        self.fail_for("inverted")

    def test_fail_regex_override_denied(self):
        self.tests[0]["properties"].append({"name": "FAIL_REGULAR_EXPRESSION", "value": ["fail"]})
        self.fail_for("inverted")

    def test_duplicate_properties_denied(self):
        self.tests[0]["properties"].append(self.tests[0]["properties"][0])
        self.fail_for("duplicate")

    def test_unknown_properties_denied(self):
        self.tests[0]["properties"].append({"name": "TIMEOUT", "value": 12})
        self.fail_for("unreviewed")

    def test_forged_guest_interpreter_denied(self):
        self.tests[0]["command"][0] = str(self.clang)
        self.fail_for("Python interpreter")

    def test_forged_guest_harness_denied(self):
        self.tests[0]["command"][1] = str(self.root / "tests/other.py")
        self.fail_for("harness was replaced")

    def test_compiler_path_drift_denied(self):
        self.tests[0]["command"][3] = str(self.clang)
        self.fail_for("compiler, fixture")

    def test_guest_fixture_path_drift_denied(self):
        self.tests[0]["command"][5] = str(self.clang)
        self.fail_for("compiler, fixture")

    def test_oracle_exit_drift_denied(self):
        self.tests[0]["command"][7] = "0"
        self.fail_for("compiler, fixture")

    def test_clang_path_drift_denied(self):
        self.tests[0]["command"][9] = str(self.root / "missing-clang")
        self.fail_for("clang executable")

    def test_missing_array_runtime_denied(self):
        array = next(t for t in self.tests if "--native-runtime" in t["command"])
        array["command"] = array["command"][:-2]
        self.fail_for("runtime")

    def test_fake_native_runtime_denied(self):
        array = next(t for t in self.tests if "--native-runtime" in t["command"])
        array["command"][-1] = str(self.clang)
        self.fail_for("runtime")

    def test_duplicate_case_denied(self):
        self.tests.append(deepcopy(self.tests[0]))
        self.fail_for("duplicate LRU")

    def test_missing_case_denied(self):
        self.tests.pop(0)
        self.fail_for("missing native")

    def test_unknown_case_denied(self):
        self.tests[0]["name"] = "atto_lru_synthetic_green"
        self.fail_for("unrecognized")

    def test_source_guard_changed_denied(self):
        self.tests[-1]["command"][1] = str(self.clang)
        self.fail_for("source-only checker")

    def test_source_guard_timeout_changed_denied(self):
        self.tests[-1]["properties"][0]["value"] = 0
        self.fail_for("integrity CTest timeout")

    def test_working_directory_drift_denied(self):
        self.tests[0]["properties"][0]["value"] = str(self.root)
        self.fail_for("working directory")

    def test_duplicate_json_keys_denied(self):
        with self.assertRaisesRegex(ValueError, "duplicate CTest JSON object key"):
            json.loads('{"tests":[], "tests":[]}', object_pairs_hook=no_duplicate_pairs)


if __name__ == "__main__":
    unittest.main()
