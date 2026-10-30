#!/usr/bin/env python3
"""Exact-source gate for callback execution-context source parity.

LLVM does not yet lower async/Future callback source. This test requires a
specific fail-closed compiler diagnostic and a working positive scalar control,
so a broken compiler cannot masquerade as an expected rejection.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

EXPECTED_FIXTURE_BLOB = "83f7db76fd6437d7d4994210e78034b93a5bff26"
EXPECTED_DIAGNOSTIC = (
    "native actor/async declaration requires actor frames, "
    "suspension-aware borrow checking and LLVM lowering"
)


def git_blob_id(path: Path) -> str:
    data = path.read_bytes()
    return hashlib.sha1(
        f"blob {len(data)}\0".encode() + data,
        usedforsecurity=False,
    ).hexdigest()


def require(condition: bool, detail: str) -> None:
    if not condition:
        raise SystemExit("FAIL: " + detail)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()

    compiler = Path(args.compiler)
    fixture = Path(args.fixture)
    require(fixture.is_file(), "callback fixture is missing")
    require(
        git_blob_id(fixture) == EXPECTED_FIXTURE_BLOB,
        "callback fixture Git blob drift",
    )

    rejected = subprocess.run(
        [str(compiler), str(fixture)],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    require(rejected.returncode == 1,
            f"unsupported callback source returned {rejected.returncode}, expected 1")
    require(not rejected.stdout,
            "unsupported callback source emitted LLVM IR/stdout")
    require(EXPECTED_DIAGNOSTIC in rejected.stderr,
            "unsupported callback source produced the wrong diagnostic")

    with tempfile.TemporaryDirectory(prefix="ores-callback-control-") as tmp:
        control = Path(tmp) / "control.ores"
        control.write_text("pub fnc main(): int { return 42; }\n")
        accepted = subprocess.run(
            [str(compiler), str(control)],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        require(accepted.returncode == 0, "scalar compiler control failed")
        require(accepted.stdout.startswith("; ModuleID"),
                "scalar compiler control emitted no LLVM module")
        require(not accepted.stderr, "scalar compiler control emitted stderr")

    print(
        "NATIVE_CALLBACK_SOURCE_EXPECTED_REJECT_PASS "
        f"fixture={EXPECTED_FIXTURE_BLOB}"
    )


if __name__ == "__main__":
    main()
