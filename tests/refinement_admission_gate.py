#!/usr/bin/env python3
"""Current native refinement boundary.

Bounded int/i64 aliases using compiler-owned min/max metadata are supported by
ores.type-constraint.v1. Runtime values of constrained aliases remain denied
until checked validation/provenance exists. All other refinement surfaces in
this matrix remain fail-closed and must emit no partial LLVM IR.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

CONTROL = "fnc main(): int { return 42; }\n"
DESCRIPTOR = "declare type Code = int with min 0 and max 5;\n"
RUNTIME_USE = (
    "declare type Code = int with min 0 and max 5;\n"
    "fnc read(Code code): int { return code; }\n"
)

UNSUPPORTED = {
    "annotation": (
        "@Range(min=0,max=5)\n"
        "declare type Percentage = int;\n"
    ),
    "string-refinement": (
        "declare type NonEmpty = string with min 1;\n"
    ),
    "where": (
        "declare type NonBlank = string where it.length > 0;\n"
    ),
    "newtype": (
        "define newtype Username(string);\n"
    ),
    "general-predicate": (
        "declare type Code = int with value gte 0;\n"
    ),
    "user-proof": (
        "declare type Proof = Valid<int, UserSchema>;\n"
    ),
}


def run(compiler: Path, source: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), *extra, str(source)],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )


def require_failed_without_ir(label: str, result: subprocess.CompletedProcess[str]) -> None:
    if result.returncode == 0:
        raise AssertionError(f"{label}: unsupported source compiled")
    if result.stdout.strip():
        raise AssertionError(f"{label}: rejection leaked LLVM IR/stdout")
    if not result.stderr.strip():
        raise AssertionError(f"{label}: rejection had no diagnostic")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="ores-refinement-current-") as tmp:
        root = Path(tmp)

        control = root / "control.ores"
        control.write_text(CONTROL, encoding="utf-8")
        positive = run(compiler, control)
        if positive.returncode != 0 or "define" not in positive.stdout:
            raise AssertionError("ordinary scalar positive control failed")

        descriptor = root / "descriptor.ores"
        descriptor.write_text(DESCRIPTOR, encoding="utf-8")
        described = run(compiler, descriptor, "--constraint-descriptors")
        if described.returncode != 0:
            raise AssertionError("supported constrained descriptor rejected: " + described.stderr)
        if described.stderr.strip():
            raise AssertionError("descriptor mode unexpectedly emitted stderr")
        fields = described.stdout.strip().split("\t")
        if len(fields) != 7:
            raise AssertionError("descriptor output shape drifted")
        if fields[:5] != ["descriptor", "ores.type-constraint.v1", "Code", "i64", "true"]:
            raise AssertionError("descriptor identity/representation drifted")

        runtime = root / "runtime-use.ores"
        runtime.write_text(RUNTIME_USE, encoding="utf-8")
        denied = run(compiler, runtime)
        require_failed_without_ir("runtime constrained alias", denied)
        if "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED" not in denied.stderr:
            raise AssertionError("runtime constrained alias failed for the wrong reason")

        for label, source in UNSUPPORTED.items():
            candidate = root / (label + ".ores")
            candidate.write_text(source, encoding="utf-8")
            require_failed_without_ir(label, run(compiler, candidate))

    print(
        "PASS native refinement boundary: descriptor syntax admitted; "
        "runtime constrained values + 6 unsupported surfaces fail closed"
    )


if __name__ == "__main__":
    main()
