#!/usr/bin/env python3
"""Real oreslang-llvmc source-admission tests, not a guest scheduling proof."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def compile_case(compiler, source, file_path):
    file_path.write_text(source, encoding="utf-8")
    return subprocess.run([compiler, str(file_path)], capture_output=True,
                          text=True, timeout=30, check=False)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    args = p.parse_args()
    rejects = [
        ("removed async", "pub async fnc bad(): int { return 1; }",
         "'async' callable modifier removed"),
        ("critical and suspends", "pub critical suspends fnc bad(): int { return 1; }",
         "'critical suspends' is forbidden"),
        ("suspends and critical", "pub suspends critical fnc bad(): int { return 1; }",
         "'critical suspends' is forbidden"),
        ("missing critical lowering", "pub critical fnc bad(): int { return 1; }",
         "critical declaration requires compiler lowering"),
        ("missing suspends lowering", "pub suspends fnc bad(): int { return 1; }",
         "suspends declaration requires"),
        ("unguarded critical block",
         "pub fnc main(): int { critical { return 1; } return 7; }",
         "critical block requires scheduler-owned"),
        ("unlowered immediate try critical",
         "pub fnc main(): int { try critical { return 1; } return 7; }",
         "try critical requires immediate nonblocking ticket lowering"),
    ]
    with tempfile.TemporaryDirectory(prefix="oreslang-critical-admission-") as tmp:
        path = Path(tmp) / "case.ores"
        for name, source, expected in rejects:
            result = compile_case(args.compiler, source, path)
            output = result.stdout + result.stderr
            if result.returncode == 0 or expected not in output:
                raise AssertionError(f"{name}: rc={result.returncode}: {output[:1000]}")
        positive = compile_case(args.compiler, "pub fnc main(): int { return 7; }", path)
        if positive.returncode != 0:
            raise AssertionError("baseline scalar compilation failed: " + positive.stderr[:1000])
    print(f"PASS LLVM source admission: {len(rejects)} negatives and baseline positive")
    print("NOTE: no critical guest execution or continuation lowering tested")


if __name__ == "__main__":
    main()
