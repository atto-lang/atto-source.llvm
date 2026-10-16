#!/usr/bin/env python3
"""Fail-closed Java keyword parity: unsupported syntax must never emit LLVM IR."""
import pathlib
import subprocess
import sys
import tempfile

CASES = {
    # Enum/pattern declaration kinds are reserved; not executable source support yet.
    "enum-function": (
        "pub fnc enum(): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "pattern-function": (
        "pub fnc pattern(): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "enum-parameter": (
        "pub fnc main(int enum): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "pattern-parameter": (
        "pub fnc main(int pattern): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "over-function": (
        "pub fnc over(): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "structural-function": (
        "pub fnc structural(): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "over-parameter": (
        "pub fnc main(int over): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "structural-parameter": (
        "pub fnc main(int structural): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "over-expression": (
        "pub fnc main(int value): int { return over; }",
        "expected int expression",
    ),
    "structural-expression": (
        "pub fnc main(int value): int { return structural; }",
        "expected int expression",
    ),
    # Exceptional controls must fail closed without emitting LLVM IR.
    "throw-function": (
        "pub fnc throw(): int { return 42; }",
        "native exceptional-control boundary unsupported",
    ),
    "raise-function": (
        "pub fnc raise(): int { return 42; }",
        "native exceptional-control boundary unsupported",
    ),
    "panic-function": (
        "pub fnc panic(): int { return 42; }",
        "native exceptional-control boundary unsupported",
    ),
    "recover-parameter": (
        "pub fnc main(int recover): int { return 42; }",
        "native exceptional-control boundary unsupported",
    ),
    "throw-expression": (
        "pub fnc main(): int { return throw; }",
        "native exceptional-control boundary unsupported",
    ),
    "panic-expression": (
        "pub fnc main(): int { return panic; }",
        "native exceptional-control boundary unsupported",
    ),
    "trap-modifier": (
        "pub trap fnc main(): int { return 42; }",
        "native exception/control effect unsupported",
    ),
    "raise-statement": (
        "pub fnc main(): int { raise 42; return 0; }",
        "native exceptional-control boundary unsupported",
    ),
    "panic-statement": (
        "pub fnc main(): int { panic 42; return 0; }",
        "native exceptional-control boundary unsupported",
    ),
    "throw-statement": (
        "pub fnc main(): int { throw 42; return 0; }",
        "native exceptional-control boundary unsupported",
    ),
    "try-statement": (
        "pub fnc main(): int { try { return 42; } catch (err) { return 0; } }",
        "expected 'finally'",
    ),
    "recover-statement": (
        "pub fnc main(): int { recover 42; return 0; }",
        "native exceptional-control boundary unsupported",
    ),
    # Unlowered execution regions must fail before producing any native IR.
    # This is rejection parity, not a claim of GPU or nested-graph execution.
    "cpu-value": (
        "pub fnc main():int{return cpu { 42 };}",
        "native cpu execution block boundary unsupported",
    ),
    "cpu-function-name": (
        "pub fnc cpu():int{return 42;}",
        "native cpu execution block boundary unsupported",
    ),
    "cpu-gpu-nested": (
        "pub fnc main():int{return cpu { gpu { 42 } };}",
        "native cpu execution block boundary unsupported",
    ),
    "gpu-cpu-nested": (
        "pub fnc main():int{return gpu { cpu { 42 } };}",
        "native gpu/quantum boundary unsupported",
    ),
    "gpu-cpu-gpu-nested": (
        "pub fnc main():int{return gpu { cpu { gpu { 42 } } };}",
        "native gpu/quantum boundary unsupported",
    ),
    "gpu-value": (
        "pub fnc main():int{return gpu { 42 };}",
        "native gpu/quantum boundary unsupported",
    ),
}


def main():
    if len(sys.argv) != 2:
        raise ValueError("expected compiler executable argument")
    with tempfile.TemporaryDirectory(prefix="oreslang-native-keywords-") as tmp:
        # A negative-only runner can pass when the compiler is broken and
        # rejects everything; require a real positive source compilation.
        control = pathlib.Path(tmp) / "control.ores"
        control.write_text("pub fnc main():int{return 42;}\n", encoding="utf-8")
        baseline = subprocess.run(
            [sys.argv[1], str(control)], capture_output=True,
            text=True, check=False, timeout=20,
        )
        if baseline.returncode != 0 or "define " not in baseline.stdout:
            raise AssertionError(f"native scalar positive control failed: {baseline!r}")
        for name, (source, diagnostic) in CASES.items():
            fixture = pathlib.Path(tmp) / (name + ".ores")
            fixture.write_text(source + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.argv[1], str(fixture)],
                capture_output=True,
                text=True,
                check=False,
                timeout=20,
            )
            if (result.returncode == 0 or result.stdout.strip()
                    or diagnostic not in result.stderr):
                raise AssertionError(
                    f"{name}: expected rejection without LLVM IR, "
                    f"returncode={result.returncode}, "
                    f"stdout={result.stdout!r}, stderr={result.stderr!r}"
                )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"native keyword parity contract failed: {exc}", file=sys.stderr)
        sys.exit(1)
