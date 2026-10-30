#!/usr/bin/env python3
"""Source-local regression tests for compiled guest CI input attestation."""
from pathlib import Path
import importlib.util
import unittest

SCRIPT = Path(__file__).with_name("ci_compiled_guest_input_guard.py")
SPEC = importlib.util.spec_from_file_location("compiled_guest_guard", SCRIPT)
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)
ROOT = Path(__file__).resolve().parents[1]


class CompiledGuestInputGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        cls.workflow = (ROOT / guard.WORKFLOW).read_text(encoding="utf-8")

    def test_live_workflow_has_all_compiler_sources_pinned(self):
        self.assertGreaterEqual(len(guard.validate(self.cmake, self.workflow)), 8)

    def test_compiler_only_changed_source_triggers_compiled_guest(self):
        bad = self.workflow.replace(
            "      - src/main/cpp/oreslang/frontend/**\n", "", 1
        )
        self.assertNotEqual(bad, self.workflow)
        with self.assertRaisesRegex(guard.GateError, "compiler-only"):
            guard.validate(self.cmake, bad)

    def test_new_cmake_compiler_source_cannot_skip_blob_attestation(self):
        needle = "    src/main/cpp/oreslang/main.cpp\n"
        bad = self.cmake.replace(
            needle, needle + "    src/main/cpp/oreslang/frontend/NewCompilerSource.cpp\n", 1
        )
        self.assertNotEqual(bad, self.cmake)
        with self.assertRaisesRegex(guard.GateError, "not blob-attested"):
            guard.validate(bad, self.workflow)

    def test_missing_compiler_blob_must_fail_closed(self):
        needle = "              src/main/cpp/oreslang/frontend/Parser.cpp \\"
        bad = self.workflow.replace(needle, "", 1)
        self.assertNotEqual(bad, self.workflow)
        with self.assertRaisesRegex(guard.GateError, "not blob-attested"):
            guard.validate(self.cmake, bad)

    def test_generated_cmake_source_requires_explicit_policy_review(self):
        needle = "    src/main/cpp/oreslang/main.cpp\n"
        bad = self.cmake.replace(needle, needle + "    ${EXTRA_COMPILER_SOURCE}\n", 1)
        self.assertNotEqual(bad, self.cmake)
        with self.assertRaisesRegex(guard.GateError, "non-literal or unsupported"):
            guard.validate(bad, self.workflow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
