#!/usr/bin/env python3
"""Fail-closed CTest JSON-v1 admission for genuine LLVM LRU guest commands.

Source-free policy gate: this verifies CTest's configured commands and
properties. It does NOT execute a compiler or establish guest parity.
"""
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path
import sys

from atto_lru_llvm_source import CASES, PREFIX

SOURCE_TEST = "atto_lru_micro_source_inventory"
RUNNER_TESTS = {
    "atto_lru_" + name.replace("-", "_"): (name, oracle, needs_array)
    for name, (_blob, oracle, needs_array) in CASES.items()
}
REQUIRED = set(RUNNER_TESTS) | {SOURCE_TEST}
DISALLOWED = {
    "WILL_FAIL", "DISABLED", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION",
    "PASS_REGULAR_EXPRESSION", "FAIL_REGULAR_EXPRESSION",
}

def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)

def no_duplicate_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate CTest JSON object key: " + key)
        result[key] = value
    return result

def resolved(value: str) -> Path:
    require(isinstance(value, str) and value.startswith("/"),
            "expected an absolute CTest command path")
    return Path(value).resolve()

def validate(inventory: dict, root: Path, build: Path, expected_clang: Path) -> None:
    require(isinstance(inventory, dict) and isinstance(inventory.get("tests"), list),
            "invalid CTest JSON-v1 inventory")
    root = root.resolve()
    build = build.resolve()
    expected_clang = expected_clang.resolve()
    require(expected_clang.is_file(), "expected clang toolchain is absent")
    require(build.is_dir(), "missing source-owned build directory")
    found = set()
    for test in inventory["tests"]:
        require(isinstance(test, dict), "invalid CTest test record")
        name = test.get("name")
        require(isinstance(name, str), "missing CTest test name")
        if not name.startswith("atto_lru_"):
            continue
        require(name in REQUIRED and name not in found,
                "unrecognized or duplicate LRU CTest: " + name)
        found.add(name)
        cmd = test.get("command")
        require(isinstance(cmd, list) and all(isinstance(a, str) for a in cmd),
                "invalid CTest command: " + name)
        require(len(cmd) >= 2 and
                resolved(cmd[0]) == Path(sys.executable).resolve(),
                "LRU CTest must launch a real Python interpreter: " + name)
        props = test.get("properties")
        require(isinstance(props, list), "missing CTest properties: " + name)
        by_name = {}
        for prop in props:
            require(isinstance(prop, dict) and isinstance(prop.get("name"), str),
                    "malformed CTest property: " + name)
            key = prop["name"]
            require(key not in by_name, "duplicate CTest property: " + key)
            by_name[key] = prop.get("value")
        require(not (DISALLOWED & set(by_name)),
                "inverted, skipped or regex-overridden CTest: " + name)
        allowed = {"WORKING_DIRECTORY"}
        if name == SOURCE_TEST:
            allowed.add("TIMEOUT")
        require(set(by_name) <= allowed,
                "unreviewed CTest properties: " + name)
        require(resolved(by_name.get("WORKING_DIRECTORY")) == build,
                "CTest working directory drift: " + name)
        if name == SOURCE_TEST:
            require(by_name.get("TIMEOUT") == 20,
                    "missing source integrity CTest timeout")
            require(len(cmd) == 2 and
                    resolved(cmd[1]) == root / "tests/atto_lru_llvm_source.py",
                    "source-only checker command drift")
            continue
        case, expected_exit, needs_array = RUNNER_TESTS[name]
        suffix = [
            "--compiler", str(build / "oreslang-llvmc"),
            "--fixture", str(root / PREFIX / (case + ".atto")),
            "--run", str(expected_exit),
            "--clang",
        ]
        require(len(cmd) in (len(suffix) + 3, len(suffix) + 5),
                "unexpected native guest argv length: " + name)
        require(resolved(cmd[1]) == root / "tests/contract.py",
                "native guest harness was replaced: " + name)
        # The original fixture path may contain tests/fixtures/../..;
        # compare the canonical path while pinning its bytes separately.
        require(cmd[2] == "--compiler" and
                resolved(cmd[3]) == build / "oreslang-llvmc" and
                cmd[4] == "--fixture" and
                resolved(cmd[5]) == root / PREFIX / (case + ".atto") and
                cmd[6:9] == ["--run", str(expected_exit), "--clang"],
                "native compiler, fixture, or exit oracle drift: " + name)
        require(resolved(cmd[9]) == expected_clang,
                "unreviewed native clang executable: " + name)
        runtime = ["--native-runtime",
                   str(root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c")]
        remainder = cmd[10:]
        require((remainder == runtime) if needs_array else (remainder == []),
                "unexpected or missing native array runtime: " + name)
    require(found == REQUIRED,
            "missing native LRU CTests: " + ", ".join(sorted(REQUIRED - found)))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("json_inventory", type=Path)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    require(args.json_inventory.is_file() and
            not args.json_inventory.is_symlink() and
            0 < args.json_inventory.stat().st_size < 64 * 1024 * 1024,
            "missing or unbounded CTest JSON receipt")
    with args.json_inventory.open(encoding="utf-8") as f:
        inventory = json.load(f, object_pairs_hook=no_duplicate_pairs)
    clang = shutil.which("clang")
    require(clang is not None, "source runner lacks clang")
    validate(inventory, args.root, args.build, Path(clang))
    print("ATTO_LLVM_LRU_CTEST_ADMISSION=PASS native_guests=4 source_guard=1")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print(f"CTEST_JSON_ADMISSION=DENIED: {exc}", file=sys.stderr)
        sys.exit(1)
