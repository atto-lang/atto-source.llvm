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
    "min-length-annotation": "@MinLength(3)\n@MaxLength(32)\ntype Username = string;\nfnc main(): int { return 42; }\n",
    "min-annotation": "@Min(1)\n@Max(65535)\ntype Port = int;\nfnc main(): int { return 42; }\n",
    "range-annotation": "@Range(min=0,max=100)\ntype Percentage = int;\nfnc main(): int { return 42; }\n",
    "schema-first-static": (
        "namespace app;\n"
        "const PortSchema = Zorde.int().min(1).max(65535);\n"
        "type Port = Zorde.infer<PortSchema>;\n"
        "fnc main(): int { return 42; }\n"
    ),
    "schema-first-dynamic": (
        "namespace app;\n"
        "const S = userValidator();\n"
        "type Port = Zorde.infer<S>;\n"
        "fnc main(): int { return 42; }\n"
    ),
    "port-keyword": "type Port = int with value gte 1 and value lte 65535;\nfnc main(): int { return 42; }\n",
    "port-symbol": "type Port = int with value >= 1 and value <= 65535;\nfnc main(): int { return 42; }\n",
    "percentage-keyword": "type Percentage = int with value gte 0 and value lte 100;\nfnc main(): int { return 42; }\n",
    "percentage-symbol": "type Percentage = int with value >= 0 and value <= 100;\nfnc main(): int { return 42; }\n",
    "legacy-ge": "type Legacy = int with value ge 1;\nfnc main(): int { return 42; }\n",
    "legacy-le": "type Legacy = int with value le 10;\nfnc main(): int { return 42; }\n",
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
