#!/usr/bin/env python3
"""Real native-compiler negative evidence for exact source aggregate kinds.

This does NOT certify struct or record code generation; each source must fail
before any LLVM IR is published until tagged lowering is implemented.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from native_aggregate_negative_receipt import verify_negative_case

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
    ("prologue-exact-alias", "fnc value(): T { type T = struct { foo: int }; return T{5}; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("prologue-readonly-record-alias", "fnc value(): T { type T = record { foo: int }; return 1; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("named-constructor-order", "fnc value(): T { type T = struct { foo: int, bar: int }; return T{bar: 2, foo: 1}; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
    ("duplicate-constructor-field", "fnc value(): T { type T = struct { foo: int }; return T{foo: 1, foo: 2}; }",
     "AGGREGATE_CONSTRUCTOR_DUPLICATE_FIELD"),
    ("unknown-constructor-field", "fnc value(): T { type T = struct { foo: int }; return T{bar: 1}; }",
     "AGGREGATE_CONSTRUCTOR_UNKNOWN_FIELD"),
    ("missing-constructor-field", "fnc value(): T { type T = struct { foo: int, bar: int }; return T{foo: 1}; }",
     "AGGREGATE_CONSTRUCTOR_MISSING_FIELD"),
    ("multi-positional-constructor", "fnc value(): T { type T = struct { foo: int, bar: int }; return T{1, 2}; }",
     "AGGREGATE_CONSTRUCTOR_POSITIONAL_MULTIFIELD"),
    ("record-alias-constructor", "fnc value(): T { type T = record { foo: int }; return T{foo: 1}; }",
     "AGGREGATE_CONSTRUCTOR_REQUIRES_STRUCT_ALIAS"),
    ("duplicate-prologue-alias", "fnc value(): T { type T = struct { foo: int }; type T = record { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_DUPLICATE"),
    ("generic-prologue-alias", "fnc value(): T { type T<U> = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_GENERIC_UNSUPPORTED"),
    ("literal-bool-into-int", "fnc value(): T { type T = struct { foo: int }; return T{foo: true}; }",
     "AGGREGATE_CONSTRUCTOR_LITERAL_TYPE_MISMATCH"),
    ("literal-int-into-bool", "fnc value(): T { type T = struct { foo: bool }; return T{foo: 1}; }",
     "AGGREGATE_CONSTRUCTOR_LITERAL_TYPE_MISMATCH"),
    ("literal-string-into-int", "fnc value(): T { type T = struct { foo: int }; return T{foo: \"x\"}; }",
     "AGGREGATE_CONSTRUCTOR_LITERAL_TYPE_MISMATCH"),
)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-aggregate-kind-") as tmp:
        root = Path(tmp)
        for name, source, expected in CASES:
            # Each fixture must start empty. No earlier fixture output may
            # influence this case's proof, including object/wasm/executable.
            directory = root / name
            directory.mkdir()
            path = directory / (name + ".ores")
            original = (source + "\n" +
                        "pub fnc main(): int { return 0; }\n").encode("utf-8")
            path.write_bytes(original)
            result = subprocess.run([args.compiler, str(path)], cwd=directory,
                                    capture_output=True, text=True, timeout=30)
            verify_negative_case(name, path, original, expected, result)
    print("native aggregate kind: 20 fail-closed compiler source checks passed")

if __name__ == "__main__":
    main()
