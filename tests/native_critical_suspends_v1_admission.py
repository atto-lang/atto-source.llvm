#!/usr/bin/env python3
"""Native compiler source admission for .atto and legacy .ores inputs.

This invokes the real LLVM compiler binary supplied by CTest. Rejecting source
is not evidence of scheduler-owned critical tickets or guest execution.
A positive compiler exit must emit native LLVM IR, not just a success status.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


NEGATIVES = [
    ("removed async", "pub async fnc bad(): int { return 1; }",
     "'async' callable modifier removed"),
    ("removed bare async", "async fnc bad(): int { return 1; }",
     "'async' callable modifier removed"),
    ("critical and suspends", "pub critical suspends fnc bad(): int { return 1; }",
     "E_CALLABLE_MODIFIERS"),
    ("suspends and critical", "pub suspends critical fnc bad(): int { return 1; }",
     "E_CALLABLE_MODIFIERS"),
    ("critical routine and suspends",
     "pub critical suspends routine bad(): int { return 1; }",
     "E_CALLABLE_MODIFIERS"),
    ("suspends routine and critical",
     "pub suspends critical routine bad(): int { return 1; }",
     "E_CALLABLE_MODIFIERS"),
    ("missing critical lowering", "pub critical fnc bad(): int { return 1; }",
     "E_CRITICAL_UNSUPPORTED"),
    ("missing suspends lowering", "pub suspends fnc bad(): int { return 1; }",
     "suspends declaration requires"),
    ("unguarded critical block",
     "pub fnc main(): int { critical { return 1; } return 7; }",
     "critical block requires scheduler-owned"),
    ("critical after scalar local",
     "pub fnc main(): int { const int value = 1; critical { return value; } return 7; }",
     "critical block requires scheduler-owned"),
    ("unlowered immediate try critical",
     "pub fnc main(): int { try critical { return 1; } return 7; }",
     "try critical requires immediate nonblocking ticket lowering"),
    ("unlowered try critical with else",
     "pub fnc main(): int { try critical { return 1; } else { return 2; } }",
     "try critical requires immediate nonblocking ticket lowering"),
]


def compile_case(compiler, source, path):
    path.write_text(source + "\n", encoding="utf-8")
    return subprocess.run([str(compiler), str(path)], cwd=path.parent,
                          capture_output=True, text=True, timeout=30, check=False)


def require_no_artifacts(directory, name, suffix):
    # If a reject writes object code or LLVM IR to disk despite nonzero exit,
    # it must not count as a fail-closed compiler admission.
    leftovers = [p.name for p in directory.iterdir()
                 if p.suffix in {".ll", ".bc", ".o", ".obj", ".so", ".wasm"}]
    if leftovers:
        raise AssertionError(
            f"{suffix} {name}: rejected compile produced artifacts {leftovers}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    if not compiler.is_file():
        raise RuntimeError("native LLVM compiler binary is not a file")

    fixtures = Path(__file__).resolve().parent / "fixtures"
    fixture_negative = (fixtures / "critical-suspends-modifier-negative.atto").read_text(
        encoding="utf-8")
    fixture_positive = (fixtures / "critical-scalar-baseline.atto").read_text(
        encoding="utf-8")
    if "'critical suspends'" in fixture_negative:
        raise AssertionError("negative fixture must contain source, not expected diagnostics")

    count = 0
    with tempfile.TemporaryDirectory(prefix="atto-llvm-critical-source-") as tmp:
        root = Path(tmp)
        for suffix in (".atto", ".ores"):
            for index, (name, source, expected) in enumerate(NEGATIVES):
                case_dir = root / f"{suffix[1:]}-{index:02d}"
                case_dir.mkdir()
                result = compile_case(compiler, source, case_dir / ("case" + suffix))
                if (result.returncode != 1 or result.stdout
                        or expected not in result.stderr
                        or "panicked" in result.stderr):
                    raise AssertionError(
                        f"{suffix} {name}: expected rc=1, empty stdout and "
                        f"diagnostic {expected!r}; observed rc={result.returncode} "
                        f"stdout={result.stdout[:300]!r} stderr={result.stderr[:500]!r}")
                require_no_artifacts(case_dir, name, suffix)
                count += 1

            # The committed AttoLang modifier-negative fixture is evaluated
            # under both extensions, so the native parser cannot pass just by
            # rejecting an unfamiliar suffix.
            fixture_dir = root / (suffix[1:] + "-fixture-negative")
            fixture_dir.mkdir()
            fixture_bad = compile_case(
                compiler, fixture_negative, fixture_dir / ("case" + suffix))
            if (fixture_bad.returncode != 1 or fixture_bad.stdout
                    or "E_CALLABLE_MODIFIERS" not in fixture_bad.stderr):
                raise AssertionError(
                    f"{suffix} fixture: compiler did not reject invalid modifier")
            require_no_artifacts(fixture_dir, "fixture negative", suffix)
            count += 1

            # This is a supported scalar native-compiler path, not a guest
            # critical path. Comments may mention forbidden modifiers.
            positive_dir = root / (suffix[1:] + "-positive")
            positive_dir.mkdir()
            result = compile_case(compiler,
                fixture_positive + "\n// critical suspends async",
                positive_dir / ("case" + suffix))
            if (result.returncode != 0 or "define" not in result.stdout
                    or "main" not in result.stdout):
                raise AssertionError(
                    f"{suffix} baseline: expected executable LLVM IR; "
                    f"rc={result.returncode} stdout={result.stdout[:500]!r} "
                    f"stderr={result.stderr[:500]!r}")

    print(f"PASS LLVM compiler admission: {count} negatives, 2 scalar IR positives")
    print("NOT EXECUTED: native critical guest tickets, awaits, actor cleanup, AOT parity")


if __name__ == "__main__":
    main()
