#!/usr/bin/env python3
"""Real native-compiler negative evidence for exact source aggregate kinds.

This does NOT certify struct or record code generation; each source must fail
before any LLVM IR is published until tagged lowering is implemented.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from native_aggregate_negative_receipt import (verify_negative_case, verify_scalar_positive_control)

CASES = (
    # The diagnostic must prove a type rejection, not merely the generic
    # native-ABI hold. Distinct directions share an identical field layout.
    ("exact-struct-return", "fnc value(record { foo: int } item): struct { foo: int } { return item; }",
     "AGGREGATE_RETURN_KIND_MISMATCH"),
    ("readonly-record-return", "fnc value(struct { foo: int } item): record { foo: int } { return item; }",
     "AGGREGATE_RETURN_KIND_MISMATCH"),
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
    # Retain typed aggregate identity in lexical locals and return
    # expressions; no same-layout struct/record erasure is allowed.
    ("local-binding-cross-kind",
     "fnc value(): int { type R = record { foo: int }; type S = struct { foo: int }; const R item = S{foo: 1}; return 1; }",
     "AGGREGATE_LOCAL_INITIALIZER_KIND_MISMATCH"),
    ("local-binding-shape-mismatch",
     "fnc value(): int { type A = struct { foo: int }; type B = struct { bar: int }; const A item = B{bar: 1}; return 1; }",
     "AGGREGATE_LOCAL_INITIALIZER_SHAPE_MISMATCH"),
    ("return-shape-mismatch",
     "fnc value(struct { bar: int } item): struct { foo: int } { return item; }",
     "AGGREGATE_RETURN_SHAPE_MISMATCH"),
    ("typed-aggregate-local-unlowered",
     "fnc value(): T { type T = struct { foo: int }; const T item = T{foo: 1}; return item; }",
     "AGGREGATE_LOWERING_UNVERIFIED"),
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

    # Language-owned builtin type names must not be redirected to a local
    # aggregate alias. These are contextual identifiers, not lexer keywords.
    ("alias-shadows-option", "fnc value(): int { type Option = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-result", "fnc value(): int { type Result = record { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-task", "fnc value(): int { type Task = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-future", "fnc value(): int { type Future = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-pointer", "fnc value(): int { type Pointer = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-array", "fnc value(): int { type Array = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-channel", "fnc value(): int { type Channel = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    ("alias-shadows-actor-ref", "fnc value(): int { type ActorRef = struct { foo: int }; return 1; }",
     "AGGREGATE_LOCAL_ALIAS_SHADOW"),
    # Bounded structural source types must fail before parsing the body.
    ("aggregate-shape-65-fields",
     "fnc value(): struct { " + ", ".join(f"f{i}: int" for i in range(65)) +
     " } { return 1; }",
     "AGGREGATE_SHAPE_FIELD_BUDGET"),
)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-aggregate-kind-") as tmp:
        root = Path(tmp)
        # A real working compiler must first admit one ordinary scalar program.
        # Keep its published LLVM IR isolated from every negative case.
        control_dir = root / "_positive_control"
        control_dir.mkdir()
        control = control_dir / "scalar-positive.ores"
        control_bytes = ("pub fnc main(): int { return 42; }" + chr(10)).encode("utf-8")
        control.write_bytes(control_bytes)
        control_result = subprocess.run([args.compiler, str(control)],
                                        cwd=control_dir, capture_output=True,
                                        text=True, timeout=30)
        verify_scalar_positive_control("scalar-positive", control,
                                       control_bytes, control_result)
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
    print(f"native aggregate kind: 1 positive scalar IR control and {len(CASES)} fail-closed compiler source checks passed")

if __name__ == "__main__":
    main()
