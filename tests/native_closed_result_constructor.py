#!/usr/bin/env python3
"""Compile genuine guest Result<void,ArrayError> constructors to tagged LLVM IR.

This is ONLY the constructor-return slice, not match/try_add/owned-array parity.
It verifies nominal LLVM IR and clang-linked native ABI execution, rather than
mistaking a source parser check or wire-codec roundtrip for executable lowering.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = """
pub fnc success(): Result<void, ArrayError> { return Ok(); }
pub fnc limited(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
pub fnc no_memory(): Result<void, ArrayError> { return Err(ArrayError.NoMemory); }
"""

HARNESS = """
#include <stdint.h>
#include <stdio.h>

typedef struct { uint8_t variant, error; } ArrayResult;
extern ArrayResult success(void);
extern ArrayResult limited(void);
extern ArrayResult no_memory(void);
_Static_assert(sizeof(ArrayResult) == 2, "unexpected tagged ABI struct layout");

int main(void) {
    const ArrayResult ok = success();
    const ArrayResult limit = limited();
    const ArrayResult memory = no_memory();
    if (ok.variant != 0 || ok.error != 0
            || limit.variant != 1 || limit.error != 1
            || memory.variant != 1 || memory.error != 2) {
        fprintf(stderr, "incorrect nominal Result constructor ABI");
        return 2;
    }
    return 0;
}
"""

NEGATIVE = {
    "wrong-ok-arity": """
        fnc wrong(): Result<void, ArrayError> { return Ok(42); }
    """,
    "wrong-error-tag": """
        fnc wrong(): Result<void, ArrayError> { return Err(ArrayError.NotThere); }
    """,
    "foreign-error": """
        fnc wrong(): Result<void, ArrayError> { return Err(OtherError.Limit); }
    """,
    "wrong-error-payload": """
        fnc wrong(): Result<void, ArrayError> { return Err(1); }
    """,
    "result-as-main": """
        pub fnc main(): Result<void, ArrayError> { return Ok(); }
    """,
    "result-local-unhandled": """
        fnc wrong(): int { val pending: Result<void,ArrayError> = Ok(); return 0; }
    """,
    "implicit-int-result": """
        fnc wrong(): int { return Ok(); }
    """,
    "duplicate-constructor-names": """
        fnc same(): Result<void, ArrayError> { return Ok(); }
        fnc same(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
    """,
    "result-collides-with-scalar": """
        fnc same(): Result<void, ArrayError> { return Ok(); }
        fnc same(): int { return 0; }
    """,
}


def checked(args, **kwargs):
    result = subprocess.run(args, capture_output=True, check=False,
                            timeout=45, **kwargs)
    if result.returncode:
        raise AssertionError(
            f"{args[0]} returned {result.returncode}: {result.stderr!r}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-tagged-result-") as temp:
        root = Path(temp)
        source = root / "result.ores"
        source.write_text(SOURCE, encoding="utf8")
        ll = root / "result.ll"
        compiled = checked([str(args.compiler), str(source)])
        ir = compiled.stdout.decode("utf8")
        for fragment in (
                "ores.result.void.array_error = type { i8, i8 }",
                "@success()", "@limited()", "@no_memory()",
                "ret %ores.result.void.array_error"):
            if fragment not in ir:
                raise AssertionError(f"missing typed LLVM IR fragment: {fragment}")
        if "i8 1, i8 1" not in ir or "i8 1, i8 2" not in ir:
            raise AssertionError("missing distinct nominal error payloads")
        if "ret i64" in ir or "ret i32" in ir:
            raise AssertionError("Result constructor accidentally lowered to scalar status")
        ll.write_bytes(compiled.stdout)
        harness = root / "harness.c"
        harness.write_text(HARNESS, encoding="utf8")
        exe = root / "result-execution"
        checked([str(args.clang), "-Werror", "-x", "ir", str(ll),
                 "-x", "c", str(harness), "-o", str(exe)])
        checked([str(exe)])
        for name, invalid_source in NEGATIVE.items():
            path = root / (name + ".ores")
            path.write_text(invalid_source, encoding="utf8")
            result = subprocess.run([str(args.compiler), str(path)],
                                    capture_output=True, check=False, timeout=30)
            if result.returncode != 1 or result.stdout:
                raise AssertionError(
                    f"{name}: invalid source emitted IR or passed: "
                    f"rc={result.returncode} stdout={result.stdout!r} "
                    f"stderr={result.stderr!r}")
        print("PASS: typed Result guest constructors emit verified struct IR and execute")


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssertionError, subprocess.TimeoutExpired) as error:
        print(f"closed Result constructor gate FAILED: {error}", file=sys.stderr)
        sys.exit(1)
