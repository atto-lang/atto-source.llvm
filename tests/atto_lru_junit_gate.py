#!/usr/bin/env python3
"""Fail-closed CTest JUnit receipt gate for eight native Atto LRU guests.

This checks a test report, not independent compiler build provenance.
It must run only AFTER a successful, nonempty native ctest invocation.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

REQUIRED = frozenset({
    "atto_lru_hash_quotient",
    "atto_lru_probe_wrap",
    "atto_lru_slot_recycle",
    "atto_lru_recency_touch",
    "atto_lru_aos_triple_reuse",
    "atto_lru_aos_pair_occupancy",
    "atto_lru_aos_padded_zero",
    "atto_lru_aosoa_tail_reset",
    "atto_lru_micro_source_inventory",
})

def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)

def verify(path: Path) -> None:
    require(path.is_file() and not path.is_symlink(),
            "missing or symlinked native CTest report")
    require(0 < path.stat().st_size <= 1024 * 1024,
            "empty or unbounded native CTest report")
    root = ET.parse(path).getroot()
    require(root.tag == "testsuite", "unexpected CTest JUnit document root")
    require(root.attrib.get("tests") == "9", "incorrect JUnit declared test count")
    for attr in ("failures", "errors", "disabled", "skipped"):
        require(root.attrib.get(attr, "0") == "0",
                "JUnit declared nonzero " + attr)
    cases = list(root.findall("testcase"))
    names = [case.attrib.get("name") for case in cases]
    require(len(cases) == 9 and set(names) == REQUIRED and
            len(set(names)) == 9,
            "wrong, missing, or duplicate native guest testcases")
    for case in cases:
        # CTest writes status="run" for actual execution. It can write
        # status="disabled" without a <skipped> child; rejecting only
        # failure/skipped tags would falsely promote a disabled test.
        require(case.attrib.get("status") == "run",
                "native guest was not actually run: " + str(case.attrib.get("name")))
        require(not any(case.find(tag) is not None
                        for tag in ("failure", "error", "skipped")),
                "failed/skipped native guest: " + str(case.attrib.get("name")))
    print("ATTO_LLVM_LRU_GUEST_EXECUTION=PASS native_guests=8 source_guards=1")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("junit", type=Path)
    args = parser.parse_args()
    verify(args.junit)
