#!/usr/bin/env python3
"""Source-free positive and negative tests for LLVM CMake static inventory.

Synthetic fixture repository only; never compiles any LLVM or Atto source.
"""
from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("verify-production-cmake-inventory.py")


class ProductionCmakeInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="llvm-cmake-inventory-fixture-")
        cls.root = Path(cls.temporary.name)
        (cls.root / "tests").mkdir()
        cls.sources = [f"tests/fixture_{i:04d}.cpp" for i in range(637)]
        for relative in cls.sources:
            (cls.root / relative).write_text("int fixture = 42;\n", encoding="utf-8")
        refs = [
            f'set(REF_{i} "{chr(36)}{{CMAKE_CURRENT_SOURCE_DIR}}/{p}")'
            if i < 183 else f"set(REF_{i} {p})"
            for i, p in enumerate(cls.sources)
        ]
        header = [
            "cmake_minimum_required(VERSION 3.20)",
            "project(synthetic_only LANGUAGES C CXX)",
            "add_executable(oreslang-llvmc tests/fixture_0000.cpp)",
            "add_executable(atto-checked-actor-ir tests/fixture_0001.cpp)",
            "add_executable(oreslang-actor-slab-heap-test tests/fixture_0002.cpp)",
            "add_executable(oreslang-actor-slab-heap-bench tests/fixture_0003.cpp)",
            "add_test(NAME native_actor_slab_heap_contract COMMAND true)",
        ]
        cls.good = "\n".join(
            header
            + [f"add_test(NAME synthetic_{i:04d} COMMAND true)" for i in range(619)]
            + refs
            + ["# " + "synthetic-padding-" * 41000]
        ) + "\n"
        assert len(cls.good.encode("utf-8")) > 600_000
        subprocess.run(
            ["git", "init", "-q", str(cls.root)], check=True, capture_output=True
        )
        subprocess.run(
            ["git", "-C", str(cls.root), "-c", "user.name=SourceFree",
             "-c", "user.email=source-free@example.invalid",
             "commit", "--allow-empty", "-m", "source-free-fixture", "-q"],
            check=True, capture_output=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def setUp(self) -> None:
        (self.root / "CMakeLists.txt").write_text(self.good, encoding="utf-8")

    def invoke(self, expected: str, ok: bool = False) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.root)],
            text=True, capture_output=True, timeout=25,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(expected, result.stdout)
            self.assertIn("native_compilation=NOT_EXECUTED", result.stdout)
        else:
            self.assertEqual(result.returncode, 78, result.stdout)
            self.assertIn(expected, result.stderr)

    def test_complete_static_fixture(self) -> None:
        self.invoke("project_source_refs=637", ok=True)

    def test_truncated_production_file_denied(self) -> None:
        (self.root / "CMakeLists.txt").write_text(
            self.good[:5000], encoding="utf-8"
        )
        self.invoke("TRUNCATED_PRODUCTION_CMAKE")

    def test_carrier_only_file_denied(self) -> None:
        (self.root / "CMakeLists.txt").write_text(
            self.good + "\n# Temporary *carrier-only* build description\n",
            encoding="utf-8",
        )
        self.invoke("CARRIER_ONLY_CMAKE_IS_NOT_PRODUCTION")

    def test_missing_test_registration_denied(self) -> None:
        (self.root / "CMakeLists.txt").write_text(
            self.good.replace(
                "add_test(NAME synthetic_0001", "omitted(NAME synthetic_0001", 1
            ), encoding="utf-8",
        )
        self.invoke("INCOMPLETE_CTEST_INVENTORY")

    def test_duplicate_compiler_target_denied(self) -> None:
        (self.root / "CMakeLists.txt").write_text(
            self.good + "# add_executable(oreslang-llvmc is duplicate\n",
            encoding="utf-8",
        )
        self.invoke("MISSING_OR_DUPLICATE_BUILD_TARGET")

    def test_missing_project_reference_denied(self) -> None:
        (self.root / "CMakeLists.txt").write_text(
            self.good.replace("tests/fixture_0636.cpp", "tests/fixture_0635.cpp"),
            encoding="utf-8",
        )
        self.invoke("TRUNCATED_PROJECT_SOURCE_FILE_INVENTORY")

    def test_missing_direct_reference_denied(self) -> None:
        needle = chr(36) + "{CMAKE_CURRENT_SOURCE_DIR}/tests/fixture_0182.cpp"
        (self.root / "CMakeLists.txt").write_text(
            self.good.replace(needle, "tests/fixture_0182.cpp"),
            encoding="utf-8",
        )
        self.invoke("TRUNCATED_DIRECT_SOURCE_FILE_INVENTORY")

    def test_nonexistent_source_file_denied(self) -> None:
        missing = self.root / self.sources[-1]
        missing.unlink()
        try:
            self.invoke("NONREGULAR_OR_MISSING_CMAKE_SOURCE_REF")
        finally:
            missing.write_text("int fixture = 42;\n", encoding="utf-8")

    def test_symlinked_leaf_denied(self) -> None:
        source = self.root / self.sources[0]
        source.unlink()
        source.symlink_to("fixture_0001.cpp")
        try:
            self.invoke("SYMLINK_CMAKE_SOURCE_REF")
        finally:
            source.unlink()
            source.write_text("int fixture = 42;\n", encoding="utf-8")

    def test_symlinked_parent_directory_denied(self) -> None:
        folder = self.root / "tests"
        renamed = self.root / "actual-fixtures"
        folder.rename(renamed)
        folder.symlink_to(renamed.name, target_is_directory=True)
        try:
            self.invoke("SYMLINK_CMAKE_SOURCE_REF")
        finally:
            folder.unlink()
            renamed.rename(folder)


if __name__ == "__main__":
    unittest.main()
