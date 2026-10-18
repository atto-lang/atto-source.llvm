#!/usr/bin/env python3
"""Reject callable-class spoofing and unsupported native inheritance, without emitting IR."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()

    negatives = []
    for name in ("Callable", "Fnc", "Function", "Routine", "Method"):
        negatives.append((f"define class {name} as end\n", "nominal class types"))
    negatives.append((
        "define actor Callable as\n"
        "  let count: int = 0;\n"
        "  pub fnc on_message(int value): int { return value; }\n"
        "end\n", "nominal actor types"))
    for clause in ("extends Callable", "extends Fnc<int, int>",
                   "implements Callable", "implements Fnc<int, int>"):
        negatives.append((f"define class Child {clause} as end\n", None))

    entry = "pub fnc main(): int { return 0; }\n"
    with tempfile.TemporaryDirectory(prefix="ores-callable-nominal-") as root:
        source = Path(root) / "boundary.ores"
        for body, diagnostic in negatives:
            source.write_text(body + entry)
            result = subprocess.run([args.compiler, str(source)],
                                    capture_output=True, text=True, timeout=30)
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError(("unsupported callable nominal form admitted", body,
                                      result.returncode, result.stdout))
            if diagnostic and diagnostic not in result.stderr:
                raise AssertionError(("missing native diagnostic", body, result.stderr))

        source.write_text(
            "define class Ordinary as\n"
            "  pub let int value = 1;\n"
            "end\n"
            "pub fnc main(): int { return 0; }\n")
        positive = subprocess.run([args.compiler, str(source)],
                                  capture_output=True, text=True, timeout=30)
        if positive.returncode or positive.stderr or not positive.stdout.startswith("; ModuleID"):
            raise AssertionError(("ordinary class affected", positive.stderr, positive.stdout))
    print(f"PASS {len(negatives)} negative callable-nominal tests and ordinary class")


if __name__ == "__main__":
    main()
