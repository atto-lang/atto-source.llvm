#!/usr/bin/env python3
"""Adversarial validation for the source-owned full-CTest manifest gate."""
import copy
import sys
import unittest

from ctest_manifest_gate import (
    CRITICAL_TESTS, GUEST_FIXTURES, ManifestFailure, NATIVE_EXECUTABLES,
    SOURCE_ACTOR_TESTS, TIMED_TESTS, check_manifest,
)


def fixture():
    # Model the exact *shape* of real CMake-generated commands. The release
    # gate must reject synthetic no-op programs under mandatory test names.
    entries = []
    for name in CRITICAL_TESTS:
        props = [{"name": "TIMEOUT", "value": 90.0}] if name in TIMED_TESTS else []
        if name in NATIVE_EXECUTABLES:
            command = ["/work/build/" + NATIVE_EXECUTABLES[name]]
            if name == "portable_target_abi_v1":
                command.append("/work/src/contracts/architecture/v1/targets.tsv")
        elif name in GUEST_FIXTURES:
            source, evidence = GUEST_FIXTURES[name]
            command = [sys.executable, "/work/src/tests/contract.py",
                       "--compiler", "/work/build/oreslang-llvmc",
                       "--fixture", "/work/src/tests/fixtures/" + source,
                       evidence, "ret i64 42" if evidence == "--ir" else "expected diagnostic"]
        elif name in SOURCE_ACTOR_TESTS:
            command = [sys.executable, "/work/src/tests/source_actor_e2e.py",
                       "--compiler", "/work/build/oreslang-llvmc",
                       "--clang", "/usr/bin/clang", "--root", "/work/src"]
            if name == "source_actor_compiled_native_asan_ubsan":
                command.append("--sanitize")
        elif name == "native_ctest_manifest_harness_adversarial":
            command = [sys.executable, "-m", "unittest", "discover",
                       "-s", "/work/src/tests", "-p", "test_ctest_manifest_gate.py", "-v"]
        elif name.startswith("native_contract_harness_"):
            command = [sys.executable]
            if name == "native_contract_harness_python_O":
                command.append("-O")
            elif name == "native_contract_harness_python_OO":
                command.append("-OO")
            command.append("/work/src/tests/test_contract_harness.py")
        elif name == "native_contract_positive_object_verifies":
            command = [sys.executable, "/work/src/tests/contract.py",
                       "--compiler", "/work/build/oreslang-llvmc",
                       "--fixture", "/work/src/tests/fixtures/constant-main.ores",
                       "--verify-object", "--clang", "/usr/bin/clang"]
        else:
            raise ValueError("unclassified critical fixture: " + name)
        entries.append({"name": name, "command": command, "properties": props})
    for index in range(260):
        entries.append({"name": f"native_extra_{index}",
                        "command": [sys.executable, "-c", "pass"]})
    return {"kind": "ctestInfo", "version": {"major": 1, "minor": 0},
            "tests": entries}

class ManifestAuditTests(unittest.TestCase):
    def setUp(self):
        self.manifest = fixture()

    def fail_on(self, expected):
        with self.assertRaisesRegex(ManifestFailure, expected):
            check_manifest(self.manifest)

    def test_complete_manifest_is_admitted(self):
        self.assertEqual(check_manifest(self.manifest), 260 + len(CRITICAL_TESTS))

    def test_empty_manifest_rejected(self):
        self.manifest["tests"] = []
        self.fail_on("expected at least")

    def test_non_json_object_rejected(self):
        with self.assertRaises(ManifestFailure):
            check_manifest("success")

    def test_invalid_tests_key_rejected(self):
        self.manifest["tests"] = {}
        self.fail_on("tests array")

    def test_required_positive_compiler_object_gate_must_exist(self):
        self.manifest["tests"] = [
            x for x in self.manifest["tests"]
            if x["name"] != "native_contract_positive_object_verifies"
        ]
        self.fail_on("required CTest was not registered")

    def test_python_optimized_gates_cannot_be_missing(self):
        for critical in CRITICAL_TESTS[:3]:
            corpus = fixture()
            corpus["tests"] = [x for x in corpus["tests"] if x["name"] != critical]
            with self.assertRaisesRegex(ManifestFailure, "required CTest"):
                check_manifest(corpus)

    def test_optimized_python_flags_are_not_optional(self):
        for name, flag in (
                ("native_contract_harness_python_O", "-O"),
                ("native_contract_harness_python_OO", "-OO")):
            document = fixture()
            entry = next(x for x in document["tests"] if x["name"] == name)
            entry["command"].remove(flag)
            with self.assertRaisesRegex(ManifestFailure, "not invoked with Python"):
                check_manifest(document)

    def test_optimized_harness_must_execute_correct_script(self):
        self.manifest["tests"][0]["command"][-1] = "tests/empty.py"
        self.fail_on("wrong script")

    def test_native_object_gates_cannot_be_omitted(self):
        for flag in ("--verify-object", "--clang", "--compiler", "--fixture"):
            document = fixture()
            entry = next(x for x in document["tests"]
                         if x["name"] == "native_contract_positive_object_verifies")
            entry["command"].remove(flag)
            with self.assertRaisesRegex(ManifestFailure, "source object CTest is missing"):
                check_manifest(document)

    def test_native_object_fixture_cannot_be_replaced_with_stub(self):
        name = "native_contract_positive_object_verifies"
        entry = next(x for x in self.manifest["tests"] if x["name"] == name)
        idx = entry["command"].index("tests/fixtures/constant-main.ores")
        entry["command"][idx] = "tests/fixtures/empty.ores"
        self.fail_on("does not compile positive guest source")

    def test_wrong_ctest_schema_version_rejected(self):
        self.manifest["version"]["major"] = 2
        self.fail_on("version/kind")

    def test_wrong_ctest_schema_kind_rejected(self):
        self.manifest["kind"] = "fakeCtTestInfo"
        self.fail_on("version/kind")

    def test_native_heap_name_cannot_run_noop_python(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_actor_owned_heap_contract")
        target["command"] = [sys.executable, "-c", "pass"]
        self.fail_on("wrong executable")

    def test_supervisor_name_cannot_run_another_native_binary(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_supervisor_priority_control")
        target["command"] = ["/work/build/oreslang-supervisor-hierarchy-test"]
        self.fail_on("wrong executable")

    def test_negative_guest_fixture_cannot_be_stubbed(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "reject_throw_effect")
        target["command"] = [sys.executable, "-c", "pass"]
        self.fail_on("wrong compiler/fixture")

    def test_native_rt_rejection_requires_exact_fixture(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_rt_while_loop_failclosed")
        idx = target["command"].index("--fixture")
        target["command"][idx + 1] = "/work/src/tests/fixtures/constant-main.ores"
        self.fail_on("wrong compiler/fixture")

    def test_native_actor_source_gate_cannot_run_fake_program(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "source_actor_compiled_native_lifecycle")
        target["command"] = [sys.executable, "-c", "pass"]
        self.fail_on("source harness")

    def test_manifest_selftest_cannot_run_noop_python(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_ctest_manifest_harness_adversarial")
        target["command"] = [sys.executable, "-c", "pass"]
        self.fail_on("does not run its test suite")

    def test_target_abi_cannot_use_unrelated_contract_file(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "portable_target_abi_v1")
        target["command"][-1] = "/work/other/targets.tsv"
        self.fail_on("wrong contract file")

    def test_python_interpreter_cannot_be_replaced_by_shell(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_contract_harness_python_O")
        target["command"][0] = "/bin/bash"
        self.fail_on("wrong interpreter/arguments")

    def test_optimized_harness_cannot_run_python_code_instead_of_script(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_contract_harness_python_O")
        target["command"][1:1] = ["-c", "print('pass')"]
        self.fail_on("wrong interpreter/arguments")

    def test_noncritical_skipped_test_is_never_silent(self):
        self.manifest["tests"][-1]["properties"] = [
            {"name": "SKIP_RETURN_CODE", "value": 77}
        ]
        self.fail_on("skip policy")

    def test_noncritical_disabled_test_is_never_silent(self):
        self.manifest["tests"][-1]["properties"] = [
            {"name": "DISABLED", "value": True}
        ]
        self.fail_on("disabled/inverted")

    def test_noncritical_unresolved_binary_is_never_silent(self):
        self.manifest["tests"][-1]["command"] = ["/tmp/BINARY-NOTFOUND"]
        self.fail_on("unresolved/empty")

    def test_optimized_python_flag_cannot_follow_script(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_contract_harness_python_O")
        target["command"].remove("-O")
        target["command"].append("-O")
        self.fail_on("not invoked with Python -O")

    def test_native_clang_option_requires_a_value(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "native_contract_positive_object_verifies")
        pos = target["command"].index("--clang")
        target["command"][pos + 1] = "--empty"
        self.fail_on("no value for --clang")

    def test_actor_sanitizer_gate_does_not_run_unsanitized(self):
        target = next(x for x in self.manifest["tests"]
                      if x["name"] == "source_actor_compiled_native_asan_ubsan")
        target["command"].remove("--sanitize")
        self.fail_on("missing --sanitize")

    def test_audit_must_not_miss_supervisor_gate(self):
        self.manifest["tests"] = [x for x in self.manifest["tests"]
            if x["name"] != "native_supervisor_hierarchy_control"]
        self.fail_on("required CTest was not registered")

    def test_duplicate_test_names_rejected(self):
        self.manifest["tests"].append(copy.deepcopy(self.manifest["tests"][0]))
        self.fail_on("duplicate CTest name")

    def test_unresolved_generator_expression_rejected(self):
        self.manifest["tests"][0]["command"] = ["$<TARGET_FILE:missing>"]
        self.fail_on("unresolved/empty")

    def test_missing_executable_rejected(self):
        self.manifest["tests"][0]["command"] = []
        self.fail_on("unresolved/empty")

    def test_cmake_notfound_executable_rejected(self):
        self.manifest["tests"][0]["command"] = ["CLANG-NOTFOUND"]
        self.fail_on("unresolved/empty")

    def test_disabled_critical_test_rejected(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "DISABLED", "value": True})
        self.fail_on("disabled/inverted")

    def test_will_fail_inverted_critical_test_rejected(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "WILL_FAIL", "value": "ON"})
        self.fail_on("disabled/inverted")

    def test_skip_return_code_is_not_positive_evidence(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "SKIP_RETURN_CODE", "value": 77})
        self.fail_on("skip policy")

    def test_skip_regex_is_not_positive_evidence(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "SKIP_REGULAR_EXPRESSION", "value": "unavailable"})
        self.fail_on("skip policy")

    def test_missing_timeout_rejected(self):
        self.manifest["tests"][0]["properties"] = []
        self.fail_on("lacks numeric timeout")

    def test_invalid_timeout_rejected(self):
        for value in (0, -1, 301, "NaN", "Infinity"):
            corpus = fixture()
            corpus["tests"][0]["properties"][0]["value"] = value
            with self.assertRaisesRegex(ManifestFailure, "timeout is unsafe"):
                check_manifest(corpus)

    def test_duplicate_properties_rejected(self):
        p = self.manifest["tests"][0]["properties"][0]
        self.manifest["tests"][0]["properties"].append(copy.deepcopy(p))
        self.fail_on("duplicate property")

    def test_minimum_count_rejected(self):
        self.manifest["tests"] = self.manifest["tests"][:10]
        self.fail_on("expected at least")

    def test_bad_entry_type_rejected(self):
        self.manifest["tests"].append(None)
        self.fail_on("valid name")


if __name__ == "__main__":
    unittest.main()
