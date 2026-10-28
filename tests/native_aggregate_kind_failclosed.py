#!/usr/bin/env python3
"""Real native-compiler negative evidence for exact source aggregate kinds.

This does NOT certify struct or record code generation; each source must fail
before any LLVM IR is published until tagged lowering is implemented.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

CASES = (
    ("exact-struct-return", "fnc value(): struct { foo: int } { return 1; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("readonly-record-return", "fnc value(): record { foo: int } { return 1; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("exact-struct-argument", "fnc value(struct { foo: int } item): int { return 1; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("readonly-record-argument", "fnc value(record { foo: int } item): int { return 1; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("duplicate-field", "fnc value(): struct { foo: int, foo: int } { return 1; }",
     "AGGREGATE_SHAPE_DUPLICATE_FIELD"),
    ("unsupported-field", "fnc value(): record { ptr: Pointer } { return 1; }",
     "AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE"),
    ("unsupported-nested", "fnc value(): struct { inner: record { foo: int } } { return 1; }",
     "AGGREGATE_SHAPE_UNSUPPORTED_FIELD_TYPE"),
)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-aggregate-kind-") as tmp:
        directory = Path(tmp)
        for name, source, expected in CASES:
            path = directory / (name + ".ores")
            path.write_text(source + "\npub fnc main(): int { return 0; }\n",
                            encoding="utf-8")
            result = subprocess.run([args.compiler, str(path)], cwd=directory,
                                    capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            if result.returncode == 0 or expected not in output:
                raise AssertionError(
                    f"{name}: expected nonzero and {expected!r}: "
                    f"rc={result.returncode} stderr={result.stderr!r}")
            if "define " in result.stdout or "target triple" in result.stdout:
                raise AssertionError(name + ": LLVM IR emitted on rejected aggregate")
            if list(directory.glob("*.ll")) or list(directory.glob("*.bc")):
                raise AssertionError(name + ": LLVM artifact emitted on rejected aggregate")
    print("native aggregate kind: seven fail-closed compiler source checks passed")

if __name__ == "__main__":
    main()
