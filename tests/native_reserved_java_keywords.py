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
    "pattern-nominal": (
        "define class pattern as val int value;end pub fnc main():int{return 0;}",
        "contextual pattern keyword cannot declare a native nominal type",
    ),
    "enum-parameter": (
        "pub fnc main(int enum): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "pattern-parameter": (
        "pub fnc main(int pattern): int { return 42; }",
        "native main() must have zero arguments",
    ),
    # Host/device scopes are currently reserved, never interpreted as i64
    # expressions or silently executed on the CPU. The same applies at
    # callable declarations and inside nested blocks.
    "gpu-callable": (
        "pub gpu fnc kernel(): int { return 42; }",
        # A gpu fnc is a typed kernel signature (void only); never CPU code.
        "GPU-LAUNCH-001: GPU kernel must return void; no implicit CPU fallback",
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
    # G0: reserve unsafe without ever emitting source-level native IR.
    "unsafe-callable": (
        "pub unsafe fnc main(): int { return 42; }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-block": (
        "pub fnc main(): int { unsafe { return 42; } }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-identifier": (
        "pub fnc unsafe(): int { return 42; }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-parameter": (
        "pub fnc main(int unsafe): int { return 42; }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-template-interpolation": (
        "pub fnc main(): int { return `value ${unsafe}`; }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-nested-template-interpolation": (
        "pub fnc main(): int { return `outer ${`inner ${unsafe}`}`; }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-after-cr-comment": (
        "pub fnc main(): int { return 42; } // ordinary comment\runsafe",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-after-crlf-comment": (
        "pub fnc main(): int { return 42; } // ordinary comment\r\nunsafe",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-in-template-after-cr-comment": (
        "pub fnc main(): int { return `value ${42 // comment\runsafe}`; }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-after-valid-function": (
        "pub fnc main(): int { return 42; } unsafe",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-qualified-member": (
        "pub fnc main(): int { return std.unsafe.memory.read(); }",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
}

# Bound native lexical admission consistently with Java/.NET/Forge.
# The template lexer has a separate block-comment scanner.
CASES.update({
    "native-block-comment-overdepth": (
        "pub fnc main(): int { " + "/*" * 129 + "unsafe" + "*/" * 129 +
        " return 42; }",
        "block comment nesting exceeds 128",
    ),
    "unsafe-after-nested-comment": (
        "pub fnc main(): int { return 42; } " +
        "/*" * 128 + "unsafe" + "*/" * 128 + " unsafe",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "unsafe-after-lf-comment": (
        "pub fnc main(): int { return 42; } // comment\nunsafe",
        "unsafe is reserved until checked unsafe effects and backend lowering exist",
    ),
    "native-template-comment-overdepth": (
        "pub fnc main(): int { return `test ${42 " +
        "/*" * 129 + "unsafe" + "*/" * 129 + "}`; }",
        "native template block comment nesting exceeds 128",
    ),
})

# Safe ordinary program text and longer identifiers must not be rejected as
# unsafe. These fixtures require actual compilation and emitted LLVM IR.
SAFE_CASES = {
    "unsafe-comment": "pub fnc main(): int { /* unsafe */ return 42; }",
    "unsafe-nested-comment-boundary": (
        "pub fnc main(): int { " + "/*" * 128 + "unsafe" +
        "*/" * 128 + " return 42; }"
    ),
    "unsafe-identifier-prefix": (
        "fnc unsafe_helper(): int { return 42; } "
        "pub fnc main(): int { return unsafe_helper(); }"
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
            if name in ("unsafe-template-interpolation",
                        "unsafe-nested-template-interpolation",
                        "unsafe-in-template-after-cr-comment",
                        "unsafe-after-cr-comment", "unsafe-after-crlf-comment",
                        "unsafe-after-lf-comment", "unsafe-after-nested-comment"):
                expected_byte = source.index("unsafe", source.index("${")) if "template" in name else source.rindex("unsafe")
                if f"at byte {expected_byte}" not in result.stderr:
                    raise AssertionError(
                        f"{name}: unsafe diagnostic must use absolute source byte "
                        f"{expected_byte}; got {result.stderr!r}"
                    )

        for name, source in SAFE_CASES.items():
            fixture = pathlib.Path(tmp) / ("valid-" + name + ".ores")
            fixture.write_text(source + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.argv[1], str(fixture)],
                capture_output=True, text=True, check=False, timeout=20)
            if result.returncode != 0 or not result.stdout.strip():
                raise AssertionError(
                    f"{name}: valid safe program was rejected or emitted no IR, "
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
