#!/usr/bin/env python3
"""Java-to-LLVM local aggregate staged executable and fail-closed contract.

Parser-only cases must reach the explicit ABI gate and emit no LLVM IR.
Admitted stack-struct cases must generate native IR and run through Clang
when available. Never confuse IR generation with native execution evidence.
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
    ap.add_argument("--clang", default="")
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
    executable = [
        ("stack single scalar", "pub fnc main(): int { struct T { int foo } val T point = T{5}; return point.foo; }", 5),
        ("stack ordered fields", "pub fnc main(): int { struct T { int x, int y } val T point = T{3, 9}; return point.y; }", 9),
        ("stack named reordered", "pub fnc main(): int { struct T { int x, int y } val T point = T{y: 9, x: 3}; return point.y; }", 9),
        ("different local nominal identities",
         "fnc first(): int { struct T { int a } val T p = T{4}; return p.a; } "
         "fnc second(): int { struct T { int b } val T p = T{10}; return p.b; } "
         "pub fnc main(): int { return first() + second(); }", 14),
    ]
    errors = [
        ("named expression order must remain explicit",
         "pub fnc main(): int { struct T { int x, int y } val T p = T{y: first(), x: 3}; return p.y; }",
         "LOCAL_CONSTRUCT_EVAL_ORDER_UNSUPPORTED"),
        ("named noninteger order must remain explicit",
         "pub fnc main(): int { struct T { int x } val T p = T{x: true}; return p.x; }",
         "LOCAL_CONSTRUCT_EVAL_ORDER_UNSUPPORTED"),
        ("unsupported local bool field",
         "pub fnc main(): int { struct T { bool x } return 7; }",
         "LOCAL_AGGREGATE_ABI_UNSUPPORTED"),
        ("unsupported local array-like field",
         "pub fnc main(): int { struct T { Array x } return 7; }",
         "LOCAL_AGGREGATE_ABI_UNSUPPORTED"),
        ("unsupported local struct escape",
         "pub fnc main(): int { struct T { int x } val T p = T{7}; return p; }",
         "native scalar type mismatch"),
        ("missing stack field", "pub fnc main(): int { struct T { int x, int y } val T p = T{5}; return p.x; }",
         "LOCAL_CONSTRUCT_ARITY"),
        ("unknown stack field", "pub fnc main(): int { struct T { int x } val T p = T{y: 5}; return p.x; }",
         "LOCAL_CONSTRUCT_UNKNOWN_FIELD"),
        ("type mismatch", "pub fnc main(): int { struct T { int x } val T p = T{true}; return p.x; }",
         "constructor parameter type mismatch"),
        ("cannot write immutable field", "pub fnc main(): int { struct T { int x } val T p = T{5}; p.x = 6; return p.x; }",
         "cannot reassign final class field"),
        ("public local return still gated", "pub fnc foo(): T { struct T { int foo } return T{5}; }",
         "LOCAL_AGGREGATE_ABI_UNSUPPORTED"),
        ("transparent alias still gated", "fnc foo(): T { type T = struct { int foo } return T{5}; }",
         "LOCAL_AGGREGATE_ABI_UNSUPPORTED"),
        ("record alias still gated", "fnc foo(): T { record T { int foo } return T{5}; }",
         "LOCAL_AGGREGATE_ABI_UNSUPPORTED"),
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
        for label, program, expected in executable:
            result = compile_guest(args.compiler, program, source)
            if result.returncode or not result.stdout.startswith("; ModuleID"):
                raise AssertionError((label, "native IR emission failed", result.returncode,
                                      result.stderr[:1000], result.stdout[:300]))
            if args.clang:
                ir = Path(temp) / "program.ll"
                exe = Path(temp) / "program"
                ir.write_text(result.stdout, encoding="utf-8")
                linked = subprocess.run([args.clang, str(ir), "-o", str(exe)],
                                        capture_output=True, text=True, timeout=50)
                if linked.returncode:
                    raise AssertionError((label, "Clang could not link native IR",
                                          linked.stderr[:1000]))
                executed = subprocess.run([str(exe)], capture_output=True, text=True,
                                          timeout=30)
                if executed.returncode != expected:
                    raise AssertionError((label, "wrong native field result", expected,
                                          executed.returncode, executed.stderr[:1000]))
        # A private opaque local struct return may lower to the existing
        # checked record handle; this is IR admission, not an externally
        # callable/public local-type ABI or proof of native runtime execution.
        private_return = compile_guest(args.compiler,
            "fnc make_point(): Point { struct Point { int x } return Point{5}; } "
            "pub fnc main(): int { return 0; }", source)
        if private_return.returncode or not private_return.stdout.startswith("; ModuleID"):
            raise AssertionError(("private local return IR admission",
                                  private_return.returncode,
                                  private_return.stderr[:1000]))
        if "ores_record_new" not in private_return.stdout:
            raise AssertionError(("private local return missing checked record region ABI",
                                  private_return.stdout[:1000]))
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
    print(f"PASS {len(executable)} native scalar stack programs"
          + (" (LLVM+Clang+OS)" if args.clang else " (IR-only; OS execution unavailable)")
          + f", {len(accepted_syntax)} gated syntax, {len(errors)} fail-closed negatives, "
            "1 private return IR, 1 baseline and 1 scope-isolation case")


if __name__ == "__main__":
    main()
