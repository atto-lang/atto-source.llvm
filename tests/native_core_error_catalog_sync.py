#!/usr/bin/env python3
"""Fail if the host-only native error descriptor table differs from the v1 manifest."""
import json
import pathlib
import re
import sys

def main() -> int:
    manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    header = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    if manifest.get("schemaVersion") != 1 or manifest.get("package") != "std/error":
        raise ValueError("unexpected error schema")
    entries = manifest["entries"]
    by_name = {e["name"]: e for e in entries}
    if len(entries) != 99 or len(by_name) != 99:
        raise ValueError("unexpected native error catalog size")
    expected = []
    for entry in entries:
        parent = entry["parent"]
        parent_code = by_name[parent]["code"] if parent is not None else -1
        expected.append((entry["name"], entry["code"], parent_code,
                         bool(entry["abstract"]), "RetryAdvisory" in entry["interfaces"],
                         "ActorApiError" in entry["interfaces"]))
    row = re.compile(r'^\s*\{"([A-Za-z]\w*)",\s*(\d+),\s*(-?\d+),\s*(true|false),\s*(true|false),\s*(true|false)\},?\s*$', re.M)
    actual = [(a, int(b), int(c), d == "true", e == "true", f == "true")
              for a, b, c, d, e, f in row.findall(header)]
    if actual != expected:
        raise ValueError(f"native descriptor catalog differs from std/error v1: {len(actual)} vs {len(expected)}")
    print(f"OK: {len(expected)} native error descriptors match std/error v1")
    return 0

if __name__ == "__main__":
    sys.exit(main())
