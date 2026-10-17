#!/usr/bin/env python3
"""Native admission gate: refined values must not be erased to their base type.

This is REJECTION parity only, until native checked construction exists.
A real positive compiler control prevents an always-failing compiler from passing.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

CONTROL = "fnc main(): int { return 42; }\n"
UNSUPPORTED = {
    "annotation": "@Size(min=1,max=32)\ntype Username = string;\nfnc main(): int { return 42; }\n",
    "with": "type NonEmpty = string with length gt 0;\nfnc main(): int { return 42; }\n",
    "where": "type NonBlank = string where it.trim().length > 0;\nfnc main(): int { return 42; }\n",
    "newtype": "define newtype Username(string) with length gte 1;\nfnc main(): int { return 42; }\n",
    "invalid-with": "type T = string with length gt 0 or length eq 10;\nfnc main(): int { return 42; }\n",
}

def run(compiler: pathlib.Path, source: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(compiler), str(source)], capture_output=True,
                          text=True, timeout=30, check=False)

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ores-refinement-") as tmp:
        directory = pathlib.Path(tmp)
        control = directory / "control.ores"
        control.write_text(CONTROL, encoding="utf-8")
        yes = run(compiler, control)
        if yes.returncode != 0 or "define" not in yes.stdout:
            raise AssertionError("native positive control failed; rejection checks are not evidence")
        for case, source in UNSUPPORTED.items():
            candidate = directory / (case + ".ores")
            candidate.write_text(source, encoding="utf-8")
            result = run(compiler, candidate)
            if result.returncode == 0:
                raise AssertionError(f"{case}: unsupported refinement compiled without enforcement")
            if result.stdout.strip():
                raise AssertionError(f"{case}: failed admission leaked partial LLVM IR/stdout")
            if not result.stderr.strip():
                raise AssertionError(f"{case}: failure had no diagnostic")
        print(f"native fail-closed refinements: {len(UNSUPPORTED)} negative cases + positive control")

if __name__ == "__main__":
    main()
