#!/usr/bin/env python3
"""Strict fail-closed parity check for the actual native std/error table.

Not a general C++ parser. The only supported initializer shape is the current
inline constexpr std::array; changes in its shape must update this check.
"""
import json
import pathlib
import re
import sys


_ENTRY = re.compile(
    r'\{"([A-Za-z][A-Za-z0-9]*)",\s*(\d+),\s*(-?\d+),\s*'
    r'(true|false),\s*(true|false),\s*(true|false)\},?'
)
_TABLE = re.compile(
    r'inline\s+constexpr\s+std::array<CoreErrorDescriptor,\s*(\d+)>'
    r'\s+core_error_catalog\s*\{\{(.*?)\}\};', re.S
)
_ALLOWED_INTERFACES = {"CodedError", "Serializable", "RetryAdvisory", "ActorApiError"}


def validate_contract(manifest: dict, native_header: str) -> int:
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1 \
            or manifest.get("package") != "std/error":
        raise ValueError("unexpected std/error schema identity")
    entries = manifest.get("entries")
    if not isinstance(entries, list) or len(entries) != 99:
        raise ValueError("std/error v1 must contain 99 descriptors")
    by_name = {}
    used_codes = set()
    expected = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError("malformed manifest descriptor")
        name, code, parent = entry.get("name"), entry.get("code"), entry.get("parent")
        abstract, interfaces = entry.get("abstract"), entry.get("interfaces")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", name) \
                or type(code) is not int or code < 0 or type(abstract) is not bool \
                or not isinstance(interfaces, list) or any(
                    not isinstance(x, str) or x not in _ALLOWED_INTERFACES
                    for x in interfaces
                ) or len(set(interfaces)) != len(interfaces):
            raise ValueError(f"malformed descriptor at index {index}")
        if name in by_name or code in used_codes:
            raise ValueError(f"duplicate error name/code: {name}")
        if index == 0:
            if name != "Error" or code != 0 or parent is not None or abstract:
                raise ValueError("noncanonical Error root")
        elif not isinstance(parent, str) or parent not in by_name or code == 0:
            raise ValueError(f"missing or forward parent: {name}")
        parent_code = by_name[parent]["code"] if parent is not None else -1
        expected.append((name, code, parent_code, abstract,
                         "RetryAdvisory" in interfaces, "ActorApiError" in interfaces))
        by_name[name] = entry
        used_codes.add(code)

    # Eliminate fake table rows in documentation/comments. A commented-out
    # initializer is NOT an active native catalog.
    without_comments = re.sub(r"/\*.*?\*/|//[^\n]*", "", native_header, flags=re.S)
    tables = _TABLE.findall(without_comments)
    if len(tables) != 1:
        raise ValueError("missing or ambiguous active native catalog initializer")
    declared_size, initializer = tables[0]
    if int(declared_size) != len(entries):
        raise ValueError("native std::array bound differs from manifest")
    native_rows = []
    for row in initializer.splitlines():
        row = row.strip()
        if not row:
            continue
        match = _ENTRY.fullmatch(row)
        if match is None:
            raise ValueError(f"unrecognized native catalog row: {row[:80]}")
        name, code, parent_code, abstract, retry, actor = match.groups()
        native_rows.append((name, int(code), int(parent_code),
                            abstract == "true", retry == "true", actor == "true"))
    if native_rows != expected:
        raise ValueError("native active initializer differs from std/error v1 manifest")
    return len(expected)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: native_core_error_catalog_sync.py manifest.json NativeCoreError.hpp")
    manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    header = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    count = validate_contract(manifest, header)
    print(f"OK: {count} active native error descriptors match std/error v1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
