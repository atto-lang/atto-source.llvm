#!/usr/bin/env python3
"""Fail-closed source-local check of LLVM compiled-guest workflow coverage.

This checks only files already on the trusted immutable checkout. It does not
execute, export, or access private sources outside the source repository.
Positive results are structural CI-policy evidence, NOT compiler/guest evidence.
"""
from __future__ import annotations

from pathlib import Path
import re
import sys

WORKFLOW = ".github/workflows/actor-execution-ledger-exact-head.yml"
GUARD = "tests/ci_compiled_guest_input_guard.py"
CPP_SOURCE = re.compile(r"src/main/cpp/oreslang/[A-Za-z0-9_./-]+\.cpp\Z")


class GateError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def compiler_inputs(cmake: str) -> tuple[str, ...]:
    match = re.search(r"(?ms)^add_executable\(\s*oreslang-llvmc\s+(.*?)^\)", cmake)
    require(match is not None, "missing directly enumerated oreslang-llvmc CMake target")
    # Reject variable/generated inputs until the provenance gate explicitly
    # models them. Silent omission would make compiler-only changes untested.
    sources = match.group(1).split()
    require(len(sources) >= 8, "compiler target has fewer sources than audited baseline")
    require(all(CPP_SOURCE.fullmatch(path) for path in sources),
            "compiler target contains a non-literal or unsupported source input")
    require(len(set(sources)) == len(sources), "duplicate compiler source")
    return tuple(sources)


def workflow_paths(workflow: str) -> set[str]:
    match = re.search(r"(?m)^  pull_request:\s*\n    paths:\s*\n((?:      - [^\n]+\n)+)", workflow)
    require(match is not None, "compiled guest workflow has no explicit pull_request.paths")
    paths = [line.strip().removeprefix("- ").strip("'\"")
             for line in match.group(1).splitlines()]
    require(len(paths) == len(set(paths)), "duplicate workflow path trigger")
    required = {
        "src/main/cpp/oreslang/main.cpp",
        "src/main/cpp/oreslang/frontend/**",
        "src/main/cpp/oreslang/types/**",
        "src/main/cpp/oreslang/backend/**",
        "CMakeLists.txt",
        WORKFLOW,
        GUARD,
    }
    require(required.issubset(paths),
            "compiler-only source/headers or CI guard changes can skip compiled guest")
    return set(paths)


def attested_inputs(workflow: str) -> set[str]:
    job = "\n  compiled-ores-guest:\n"
    step = "      - name: Verify source identity and compiled guest gate blobs\n"
    require(workflow.count(job) == 1, "compiled-ores-guest job missing or duplicated")
    content = workflow.split(job, 1)[1]
    require(content.count(step) == 1, "source-identity step missing or duplicated")
    identity = content.split(step, 1)[1].split("\n      - name:", 1)[0]
    require("persist-credentials: false" in content.split(step, 1)[0],
            "compiled guest checkout retains credentials")
    require("test -z \"$(git status --porcelain --untracked-files=all)\"" in identity,
            "checked-out source may be dirty before blob attestation")
    require("git hash-object \"$path\"" in identity and
            "git rev-parse \"HEAD:$path\"" in identity,
            "identity step must verify each checked-out blob")
    marker = "          for path in \\\n"
    require(identity.count(marker) == 1, "expected one attestation path loop")
    remainder = identity.split(marker, 1)[1].splitlines()
    paths: list[str] = []
    finished = False
    for line in remainder:
        part = line.strip()
        if part.endswith("\\"):
            part = part[:-1].strip()
        elif part.endswith("; do"):
            part = part[:-4].strip()
            finished = True
        else:
            raise GateError("non-literal attestation path element")
        require(bool(part) and not part.startswith("#") and " " not in part,
                "malformed attestation path element")
        paths.append(part)
        if finished:
            break
    require(finished and len(set(paths)) == len(paths),
            "missing loop terminator or duplicate attestation path")
    return set(paths)


def validate(cmake: str, workflow: str) -> tuple[str, ...]:
    sources = compiler_inputs(cmake)
    workflow_paths(workflow)
    attested = attested_inputs(workflow)
    required = set(sources) | {"CMakeLists.txt", WORKFLOW, GUARD}
    require(required.issubset(attested),
            "compiler target, workflow or policy-guard input is not blob-attested")
    return sources


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    try:
        inputs = validate((root / "CMakeLists.txt").read_text(encoding="utf-8"),
                          (root / WORKFLOW).read_text(encoding="utf-8"))
    except (GateError, OSError, UnicodeError) as exc:
        print("COMPILED_GUEST_INPUT_GATE_DENIED:", exc, file=sys.stderr)
        return 1
    print("COMPILED_GUEST_INPUT_COVERAGE_OK compiler_inputs=", len(inputs),
          "STRUCTURAL_ONLY_NOT_EXECUTED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
