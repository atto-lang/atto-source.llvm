#!/usr/bin/env python3
"""Fail-closed Java keyword parity: unsupported syntax must never emit LLVM IR."""
import json
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
    # Host/device scopes are currently reserved, never interpreted as i64
    # expressions or silently executed on the CPU. The same applies at
    # callable declarations and inside nested blocks.
    "gpu-callable": (
        "pub gpu fnc kernel(): int { return 42; }",
        "native gpu/quantum boundary unsupported",
    ),
    "cpu-callable": (
        "pub cpu fnc kernel(): int { return 42; }",
        "native cpu execution block boundary unsupported",
    ),
    "cpu-block": (
        "pub fnc main(): int { cpu { return 7; } return 42; }",
        "native cpu execution block boundary unsupported",
    ),
    "cpu-task-block": (
        "pub fnc main(): int { return cpu task { 42 }; }",
        "native cpu execution block boundary unsupported",
    ),
    "gpu-task-block": (
        "pub fnc main(): int { return gpu task { 42 }; }",
        "native gpu/quantum boundary unsupported",
    ),
    "default-task-block": (
        "pub fnc main(): int { return task { 42 }; }",
        "native execution task lowering unsupported",
    ),
    "gpu-block": (
        "pub fnc main(): int { gpu { return 7; } return 42; }",
        "native gpu/quantum boundary unsupported",
    ),
    "gpu-in-cpu": (
        "pub fnc main(): int { cpu { gpu { return 7; } } return 42; }",
        "native cpu execution block boundary unsupported",
    ),
    "cpu-in-gpu": (
        "pub fnc main(): int { gpu { cpu { return 7; } } return 42; }",
        "native gpu/quantum boundary unsupported",
    ),
    "gpu-identifier": (
        "pub fnc gpu(): int { return 42; }",
        "native gpu/quantum boundary unsupported",
    ),
    "cpu-identifier": (
        "pub fnc cpu(): int { return 42; }",
        "native cpu execution block boundary unsupported",
    ),
    "quantum-callable": (
        "pub quantum fnc kernel(): int { return 42; }",
        "native gpu/quantum boundary unsupported",
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
    # Independent Task admission must reject at the TASK boundary before any
    # borrowed owner or continuation can reach LLVM IR. This is not evidence
    # that the native borrow checker can lower borrowed Tasks yet.
    "task-captured-shared-loan": (
        "pub fnc main(int x): int { return task { return rt borrow x; }; }",
        "native execution task lowering unsupported",
    ),
    "task-captured-exclusive-loan": (
        "pub fnc main(int x): int { return task { return rt borrow mut x; }; }",
        "native execution task lowering unsupported",
    ),
    "task-borrowed-call-result": (
        "fnc read(borrow int x): int { return x; } "
        "pub fnc main(int x): int { return task { return read(rt borrow x); }; }",
        "native execution task lowering unsupported",
    ),
    "task-borrowed-classic-for-initializer": (
        "pub fnc main(int owner): int { "
        "return task { for (val loop = rt borrow owner; false; ) {} return 0; }; }",
        "native execution task lowering unsupported",
    ),
    "nested-task-borrow": (
        "pub fnc main(int x): int { return task { return task { return rt borrow x; }; }; }",
        "native execution task lowering unsupported",
    ),
    "task-comment-borrow": (
        "pub fnc main(int x): int { return task /* comment */ { return rt borrow x; }; }",
        "native execution task lowering unsupported",
    ),
    "cpu-task-borrow": (
        "pub fnc main(int x): int { return cpu task { return rt borrow x; }; }",
        "native cpu execution block boundary unsupported",
    ),
    "gpu-task-borrow": (
        "pub fnc main(int x): int { return gpu task { return rt borrow x; }; }",
        "native gpu/quantum boundary unsupported",
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
}


def main():
    if len(sys.argv) != 2:
        raise ValueError("expected compiler executable argument")
    with tempfile.TemporaryDirectory(prefix="oreslang-native-keywords-") as tmp:
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

        contract = pathlib.Path(__file__).resolve().parents[1] / (
            "contracts/v2/cpu-gpu-task-unsupported-v2.json")
        shared = json.loads(contract.read_text(encoding="utf-8"))
        if len(shared) != 10:
            raise AssertionError("CPU/GPU task v2 corpus must contain exactly 10 cases")
        for name, source in shared.items():
            fixture = pathlib.Path(tmp) / ("v2-" + name + ".ores")
            fixture.write_text(source + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.argv[1], str(fixture)],
                capture_output=True,
                text=True,
                check=False,
                timeout=20,
            )
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError(
                    f"{name}: v2 execution form admitted or emitted LLVM IR, "
                    f"returncode={result.returncode}, stdout={result.stdout!r}, "
                    f"stderr={result.stderr!r}"
                )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"native keyword parity contract failed: {exc}", file=sys.stderr)
        sys.exit(1)
