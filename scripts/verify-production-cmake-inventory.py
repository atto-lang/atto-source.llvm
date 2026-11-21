#!/usr/bin/env python3
"""Source-free static inventory for restored Atto LLVM production CMake.

This is NOT a CMake configure, build, CTest, or native guest execution.
Fail closed if expected production declarations or source references disappear.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

MIN_TEST_REGISTRATIONS = 620
MIN_DIRECT_SOURCE_REFS = 183
MIN_PROJECT_SOURCE_REFS = 637
EXPECTED_TARGET_MARKERS = (
    "add_executable(oreslang-llvmc",
    "add_executable(atto-checked-actor-ir",
    "add_executable(oreslang-actor-slab-heap-test",
    "add_executable(oreslang-actor-slab-heap-bench",
    "add_test(NAME native_actor_slab_heap_contract",
)
DIRECT_SOURCE_REF = re.compile(
    r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/"
    r"([A-Za-z0-9_./-]+\.(?:cpp|hpp|cmake|json|atto|ores|py|sh|cc|h|c))"
    r"(?![A-Za-z0-9_])"
)
PROJECT_SOURCE_REF = re.compile(
    r"(?<![A-Za-z0-9_.-])"
    r"((?:(?:src|tests|ci|contracts|native|runtime|frontend|stdlib|spec|"
    r"examples|tools|scripts|verification|library|fixtures|formal)/)"
    r"[\w.+/@-]+(?:/[\w.+/@-]+)*\."
    r"(?:cpp|cc|c|cxx|h|hpp|ll|py|json|sh|cmake|atto|ores))"
    r"(?![A-Za-z0-9_.-])"
)


def assert_regular_local_file(root: Path, relative: str) -> None:
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise RuntimeError(f"UNSAFE_CMAKE_SOURCE_REF:{relative}")
    current = root
    for component in path.parts:
        current = current / component
        if current.is_symlink():
            raise RuntimeError(f"SYMLINK_CMAKE_SOURCE_REF:{relative}")
    if not current.is_file():
        raise RuntimeError(f"NONREGULAR_OR_MISSING_CMAKE_SOURCE_REF:{relative}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    cmake_path = root / "CMakeLists.txt"
    if not cmake_path.is_file() or cmake_path.is_symlink():
        raise RuntimeError("RESTORED_CMAKE_MISSING_OR_SYMLINK")
    content = cmake_path.read_text(encoding="utf-8")
    if "Temporary *carrier-only* build description" in content:
        raise RuntimeError("CARRIER_ONLY_CMAKE_IS_NOT_PRODUCTION")
    if len(content.encode("utf-8")) < 600_000:
        raise RuntimeError("TRUNCATED_PRODUCTION_CMAKE")
    registration_count = len(re.findall(r"\badd_test\(", content))
    if registration_count < MIN_TEST_REGISTRATIONS:
        raise RuntimeError(f"INCOMPLETE_CTEST_INVENTORY:{registration_count}")
    for marker in EXPECTED_TARGET_MARKERS:
        if content.count(marker) != 1:
            raise RuntimeError(f"MISSING_OR_DUPLICATE_BUILD_TARGET:{marker}")
    direct_references = set(DIRECT_SOURCE_REF.findall(content))
    project_references = set(PROJECT_SOURCE_REF.findall(content))
    if len(direct_references) < MIN_DIRECT_SOURCE_REFS:
        raise RuntimeError(
            f"TRUNCATED_DIRECT_SOURCE_FILE_INVENTORY:{len(direct_references)}"
        )
    if len(project_references) < MIN_PROJECT_SOURCE_REFS:
        raise RuntimeError(
            f"TRUNCATED_PROJECT_SOURCE_FILE_INVENTORY:{len(project_references)}"
        )
    for relative in sorted(direct_references | project_references):
        assert_regular_local_file(root, relative)
    head = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if not re.fullmatch(r"[a-f0-9]{40}", head):
        raise RuntimeError("INVALID_SOURCE_HEAD")
    print(
        f"LLVM_CMAKE_INVENTORY_STATIC_OK|head={head}|tests={registration_count}"
        f"|direct_source_refs={len(direct_references)}"
        f"|project_source_refs={len(project_references)}"
        f"|native_compilation=NOT_EXECUTED"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        print(f"LLVM_CMAKE_INVENTORY_DENIED:{exc}", file=sys.stderr)
        sys.exit(78)
