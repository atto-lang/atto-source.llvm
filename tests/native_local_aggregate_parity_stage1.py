#!/usr/bin/env python3
"""Java-to-LLVM local aggregate stage-1 parser and fail-closed admission contract.

Positive cases mean syntax survives the parser until the explicit ABI gate;
they DO NOT indicate executable guest source. The compiler must never produce
even partial LLVM IR for an unverified local aggregate.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def compile_guest(compiler, source, filename):
    filename.write_text(source, encoding="utf-8")
    return subprocess.run([compiler, str(filename)], capture_output=True,
                          text=True, timeout=40)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    args = ap.parse_args()

    accepted_syntax = [
        ("fnc named struct", "pub fnc foo(): T { struct T { int foo } return T{5}; }"),
        ("fnc transparent struct", "pub fnc foo(): T { type T = struct { int foo } return T{5}; }"),
        ("routine named struct", "pub routine foo(): T { struct T { int foo } return T{5}; }"),
        ("routine transparent struct", "pub routine foo(): T { type T = struct { int foo } return T{5}; }"),
        ("fnc record", "pub fnc foo(): T { record T { int foo } return T{5}; }"),
        ("fnc record alias", "pub fnc foo(): T { type T = record { int foo } return T{5}; }"),
        ("routine record alias", "pub routine foo(): T { type T = record { int foo } return T{5}; }"),
        ("named constructor", "pub fnc foo(): T { struct T { foo: int } return T{foo: 5}; }"),
        ("ordered fields", "pub fnc foo(): T { struct T { int x, int y } return T{3, 4}; }"),
        ("explicit semicolon", "pub fnc foo(): T { struct T { int foo }; return T{5}; }"),
    ]
    errors = [
        ("redundant const", "pub fnc foo(): T { const type T = struct { int foo } return T{5}; }",
         "LOCAL_TYPE_CONST_REDUNDANT"),
        ("duplicate name", "pub fnc foo(): T { struct T { int foo } record T { int foo } return T{5}; }",
         "LOCAL_TYPE_DUPLICATE"),
        ("duplicate field", "pub fnc foo(): T { struct T { int foo, int foo } return T{5}; }",
         "LOCAL_TYPE_FIELD_INVALID"),
        ("duplicate named constructor field",
         "pub fnc foo(): T { struct T { int foo } return T{foo: 5, foo: 6}; }",
         "LOCAL_CONSTRUCT_DUPLICATE"),
        ("mixed constructor",
         "pub fnc foo(): T { struct T { int foo, int bar } return T{5, bar: 6}; }",
         "LOCAL_CONSTRUCT_MIXED"),
        ("local class",
         "pub fnc foo(): T { class T as end return T{5}; }",
         "LOCAL_CLASS_FORBIDDEN"),
        ("local defined class",
         "pub fnc foo(): T { define class T as end return T{5}; }",
         "LOCAL_CLASS_FORBIDDEN"),
        ("unsupported alias target",
         "pub fnc foo(): T { type T = int; return T{5}; }",
         "LOCAL_TYPE_TARGET_UNSUPPORTED"),
        ("unsupported nested lexical type",
         "pub fnc foo(): int { if true { struct T { int foo } } fi return 0; }",
         "LOCAL_TYPE_NESTED_UNSUPPORTED"),
    ]

    with tempfile.TemporaryDirectory(prefix="ores-native-local-types-") as temp:
        source = Path(temp) / "local.ores"
        for label, program in accepted_syntax:
            result = compile_guest(args.compiler, program, source)
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError((label, "unverified ABI emitted LLVM IR", result.stdout[:500]))
            if "LOCAL_AGGREGATE_ABI_UNSUPPORTED" not in result.stderr:
                raise AssertionError((label, "parser/validator did not reach exact ABI gate",
                                      result.returncode, result.stderr[:1000]))
        for label, program, diagnostic in errors:
            result = compile_guest(args.compiler, program, source)
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError((label, "malformed aggregate emitted LLVM IR", result.stdout[:500]))
            if diagnostic not in result.stderr:
                raise AssertionError((label, "wrong diagnostic", diagnostic, result.stderr[:1000]))

        baseline = compile_guest(args.compiler, "pub fnc main(): int { return 7; }", source)
        if baseline.returncode or not baseline.stdout.startswith("; ModuleID"):
            raise AssertionError(("regression baseline rejected", baseline.returncode,
                                  baseline.stderr[:500], baseline.stdout[:500]))

        leak = compile_guest(args.compiler,
                            "fnc first(): T { struct T { int foo } return T{5}; } "
                            "pub fnc main(): int { return T{5}; }", source)
        if leak.returncode == 0 or leak.stdout.strip():
            raise AssertionError(("callable local type leaked or unverified IR emitted", leak.stdout[:500]))
    print(f"PASS {len(accepted_syntax)} gated syntax, {len(errors)} fail-closed negatives, "
          "1 baseline and 1 scope-isolation case")


if __name__ == "__main__":
    main()
