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
fnc success(): Result<void, ArrayError> { return Ok(); }
fnc limited(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
fnc no_memory(): Result<void, ArrayError> { return Err(ArrayError.NoMemory); }
"""

LLVM_CALLER = """
; The caller is deliberately compiled from LLVM IR with the SAME exact
; nominal aggregate type. A C struct return is NOT a verified FFI ABI.
define i32 @main() {
entry:
  %a = call %ores.result.void.array_error @success()
  %b = call %ores.result.void.array_error @limited()
  %c = call %ores.result.void.array_error @no_memory()
  %a0 = extractvalue %ores.result.void.array_error %a, 0
  %a1 = extractvalue %ores.result.void.array_error %a, 1
  %b0 = extractvalue %ores.result.void.array_error %b, 0
  %b1 = extractvalue %ores.result.void.array_error %b, 1
  %c0 = extractvalue %ores.result.void.array_error %c, 0
  %c1 = extractvalue %ores.result.void.array_error %c, 1
  %ac0 = icmp eq i8 %a0, 0
  %ac1 = icmp eq i8 %a1, 0
  %bc0 = icmp eq i8 %b0, 1
  %bc1 = icmp eq i8 %b1, 1
  %cc0 = icmp eq i8 %c0, 1
  %cc1 = icmp eq i8 %c1, 2
  %one = and i1 %ac0, %ac1
  %two = and i1 %bc0, %bc1
  %three = and i1 %cc0, %cc1
  %four = and i1 %one, %two
  %all = and i1 %four, %three
  %rc = select i1 %all, i32 0, i32 2
  ret i32 %rc
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
        fnc main(): Result<void, ArrayError> { return Ok(); }
    """,
    "unverified-public-export": """
        pub fnc exposed(): Result<void, ArrayError> { return Ok(); }
    """,
    "nominal-error-class-spoof": """
        define class ArrayError as
          int value: 1
        end
        fnc wrong(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
    """,
    "result-module-shadow": """
        define module Result as
          fnc wrong(): Result<void, ArrayError> { return Ok(); }
        end
    """,
    "error-module-shadow": """
        define module ArrayError as
          fnc wrong(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
        end
    """,
    "constructor-count-exceeds-limit": (
        "fnc f0(): Result<void, ArrayError> { return Ok(); }\n"
        + "".join(f"fnc f{i}(): Result<void, ArrayError> "
                  "{ return Ok(); }\n" for i in range(1, 65))
    ),
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
                "define internal %ores.result.void.array_error @success()",
                "define internal %ores.result.void.array_error @limited()",
                "define internal %ores.result.void.array_error @no_memory()",
                "ret %ores.result.void.array_error"):
            if fragment not in ir:
                raise AssertionError(f"missing typed LLVM IR fragment: {fragment}")
        if "i8 1, i8 1" not in ir or "i8 1, i8 2" not in ir:
            raise AssertionError("missing distinct nominal error payloads")
        if "ret i64" in ir or "ret i32" in ir:
            raise AssertionError("Result constructor accidentally lowered to scalar status")
        ll.write_bytes(compiled.stdout + b"\n" + LLVM_CALLER.encode("utf8"))
        for optimization in ("-O0", "-O2"):
            exe = root / ("result-execution-" + optimization[1:])
            checked([str(args.clang), "-Werror", "-Wno-override-module",
                     optimization, "-x", "ir", str(ll), "-o", str(exe)])
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
