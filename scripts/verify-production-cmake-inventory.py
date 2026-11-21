#!/usr/bin/env python3
"""Source-free structural admission for restored Atto LLVM production CMake.

This is NOT a CMake configure, build, CTest or native guest execution.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

MIN_TEST_REGISTRATIONS = 620
MIN_LITERAL_SOURCE_REFS = 183
EXPECTED_TARGET_MARKERS = (
    "add_executable(oreslang-llvmc",
    "add_executable(atto-checked-actor-ir",
    "add_executable(oreslang-actor-slab-heap-test",
    "add_executable(oreslang-actor-slab-heap-bench",
    "add_test(NAME native_actor_slab_heap_contract",
)
SOURCE_REF = re.compile(
    r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/"
    r"([A-Za-z0-9_./-]+\.(?:cpp|hpp|cmake|json|atto|ores|py|sh|cc|h|c))"
    r"(?![A-Za-z0-9_])"
)


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
    references = set(SOURCE_REF.findall(content))
    if len(references) < MIN_LITERAL_SOURCE_REFS:
        raise RuntimeError(f"TRUNCATED_SOURCE_FILE_INVENTORY:{len(references)}")
    for relative in sorted(references):
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts:
            raise RuntimeError(f"UNSAFE_CMAKE_SOURCE_REF:{relative}")
        target = root / path
        if not target.is_file() or target.is_symlink():
            raise RuntimeError(f"NONREGULAR_OR_MISSING_CMAKE_SOURCE_REF:{relative}")
    head = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if not re.fullmatch(r"[a-f0-9]{40}", head):
        raise RuntimeError("INVALID_SOURCE_HEAD")
    print(
        f"LLVM_CMAKE_INVENTORY_STATIC_OK|head={head}|tests={registration_count}"
        f"|direct_source_refs={len(references)}|native_compilation=NOT_EXECUTED"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        print(f"LLVM_CMAKE_INVENTORY_DENIED:{exc}", file=sys.stderr)
        sys.exit(78)
