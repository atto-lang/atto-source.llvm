#!/usr/bin/env python3
"""Actual Oreslang compiler -> LLVM IR -> Clang checked-i64 ABI integration.

This experimental ABI is narrowly restricted to one PURE scalar checked()
function; it does not claim actor cleanup, arbitrary call graphs, or ordinary
Oreslang function overflow parity. No Oreslang repository source is exported.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

SYMBOL = "__oreslang_checked_scalar_v1"


def execute(argv, *, input=None, timeout=30):
    result = subprocess.run(argv, input=input, text=True, capture_output=True,
                            timeout=timeout, check=False)
    if result.returncode:
        raise AssertionError(
            "command failed (exit %d): %s: %s" %
            (result.returncode, argv[0], result.stderr[:1500]))
    return result.stdout


def run_case(compiler, clang, folder, name, expr, count, assertions):
    source = folder / (name + ".ores")
    parameters = ",".join("int " + x for x in ("a", "b")[:count])
    source.write_text(
        "fnc checked(" + parameters + "):int{return " + expr + ";}\n",
        encoding="utf-8")
    ir = execute([compiler, "--target=host", "--experimental-checked-i64", str(source)])
    signature = r"(?m)^define\s+i32\s+@" + SYMBOL + r"\s*\("
    if not re.search(signature, ir):
        raise AssertionError(name + ": checked scalar ABI not emitted")
    for intrinsic in ("llvm.sadd.with.overflow.i64", "llvm.ssub.with.overflow.i64",
                      "llvm.smul.with.overflow.i64"):
        if (("+" in expr and intrinsic.endswith("sadd.with.overflow.i64")) or
                ("*" in expr and intrinsic.endswith("smul.with.overflow.i64")) or
                ("-" in expr and intrinsic.endswith("ssub.with.overflow.i64"))):
            if intrinsic not in ir:
                raise AssertionError(name + ": missing " + intrinsic)
    irfile = folder / (name + ".o")
    execute([clang, "-x", "ir", "-c", "-o", str(irfile), "-"], input=ir)
    arguments = ",".join("int64_t " + x for x in ("a", "b")[:count])
    signature_args = arguments + ("," if arguments else "") + "int64_t *out"
    code = [
        "#include <stdint.h>",
        "#include <inttypes.h>",
        "#include <limits.h>",
        "#include <stdio.h>",
        "extern int32_t " + SYMBOL + "(" + signature_args + ");",
        "int main(void) {",
    ]
    for index, (a, b, expected_status, expected_value) in enumerate(assertions):
        values = [] if count == 0 else [a] if count == 1 else [a, b]
        args = ",".join(values) + ("," if values else "")
        code += [
            "  int64_t out%d = INT64_C(987654);" % index,
            "  int32_t status%d = %s(%s&out%d);" % (index, SYMBOL, args, index),
            "  if (status%d != %d) return %d;" % (index, expected_status, index + 1),
        ]
        if expected_status == 0:
            code.append("  if (out%d != %s) return %d;" %
                        (index, expected_value, index + 40))
        else:
            code.append("  if (out%d != INT64_C(987654)) return %d;" %
                        (index, index + 60))
    null_args = ",".join(["INT64_C(1)"] * count) + ("," if count else "")
    code.append("  if (%s(%s0) != 2) return 90;" % (SYMBOL, null_args))
    code += ['  puts("CHECKED_SCALAR_' + name.upper() + '=PASS");',
             "  return 0;", "}"]
    host = folder / (name + ".c")
    host.write_text("\n".join(code) + "\n", encoding="utf-8")
    binary = folder / name
    execute([clang, "-std=c11", "-Wall", "-Wextra", "-Werror", str(host),
             str(irfile), "-o", str(binary)])
    result = execute([str(binary)])
    if result.strip() != "CHECKED_SCALAR_" + name.upper() + "=PASS":
        raise AssertionError(name + ": wrong native test output " + result)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    opts = p.parse_args()
    compiler = str(pathlib.Path(opts.compiler).resolve(strict=True))
    clang = str(pathlib.Path(opts.clang).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="ores-checked-scalar-") as d:
        folder = pathlib.Path(d)
        # Boundary inputs test a real *runtime* decision, not compiler
        # rejection of a trivially constant overflow expression.
        suites = [
            ("add", "a+b", 2, [
                ("INT64_MAX", "INT64_C(-1)", 0, "INT64_MAX-1"),
                ("INT64_MAX", "INT64_C(1)", 1, None),
                ("INT64_MIN", "INT64_C(-1)", 1, None)]),
            ("subtract", "a-b", 2, [
                ("INT64_MIN", "INT64_C(-1)", 0, "INT64_MIN+1"),
                ("INT64_MIN", "INT64_C(1)", 1, None),
                ("INT64_MAX", "INT64_C(-1)", 1, None)]),
            ("multiply", "a*b", 2, [
                ("INT64_C(3)", "INT64_C(-7)", 0, "INT64_C(-21)"),
                ("INT64_MAX", "INT64_C(2)", 1, None),
                ("INT64_MIN", "INT64_C(-1)", 1, None)]),
            ("negate", "-a", 1, [
                ("INT64_C(42)", "", 0, "INT64_C(-42)"),
                ("INT64_MIN", "", 1, None)]),
            ("intermediate", "(a+b)*3", 2, [
                ("INT64_C(4)", "INT64_C(8)", 0, "INT64_C(36)"),
                ("INT64_MAX", "INT64_C(1)", 1, None),
                ("INT64_MAX", "INT64_C(-1)", 1, None)]),
        ]
        for args in suites:
            run_case(compiler, clang, folder, *args)
        for name, source in [
            ("unsupported-locals", "fnc checked(int a):int{val int x:a;return x;}"),
            ("unsupported-calls", "fnc checked(int a):int{return other(a);}"),
            ("wrong-function", "fnc nonchecked(int a):int{return a;}"),
        ]:
            f = folder / (name + ".ores")
            f.write_text(source + "\n", encoding="utf-8")
            p = subprocess.run([compiler, "--experimental-checked-i64", str(f)],
                               capture_output=True, text=True, timeout=15)
            if p.returncode != 1 or not p.stderr.startswith("oreslang-llvmc:"):
                raise AssertionError(name + ": unsupported program not explicitly rejected")
    print("NATIVE_CHECKED_SCALAR_SUITES=5 NEGATIVE=3 ABI_STATUS=0,1,2")


if __name__ == "__main__":
    main()
