#!/usr/bin/env python3
"""Exact queue source admission, anti-false-positive controls, and native smoke.

A passing gate proves that current LLVM intentionally refuses the six pinned
queue programs without emitting IR. It does not prove native queue parity.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


# SHA-1 of the full Git blob header + contents, independently pinned to the
# queue repository's merged 2df46251 commit. An edited snapshot needs review.
FIXTURES = {
    "src/int_linked_queue.ores": (
        "2a250d3d8721ecd0817dce6c3adc50b5dfe7f52f",
        "oreslang-llvmc: expected 'module', 'class' or 'actor' at byte 311",
    ),
    "tests/smoke.ores": (
        "55c417dde5210dc71666b256d7ee90bce9f812f2",
        "oreslang-llvmc: expected fnc or routine at byte 156",
    ),
    "tests/stress.ores": (
        "90759f6cc630c8c5f6ce2cfdd662211cdffad813",
        "oreslang-llvmc: expected fnc or routine at byte 124",
    ),
    "tests/edge_cases.ores": (
        "3ef072c04f4176e3d786c447790c6fb254e6b04b",
        "oreslang-llvmc: expected fnc or routine at byte 82",
    ),
    "tests/instances.ores": (
        "6bcc4396862b6f87412e6a78a4fea31be2f37df8",
        "oreslang-llvmc: expected fnc or routine at byte 143",
    ),
    "tests/relink_matrix.ores": (
        "52b40ceac8a9f8dd948ee93093e7b33583e3bf36",
        "oreslang-llvmc: expected fnc or routine at byte 67",
    ),
}


def git_blob_sha(content):
    return hashlib.sha1(
        b"blob " + str(len(content)).encode("ascii") + b"\0" + content
    ).hexdigest()


def verify_fixture(source, expected_sha):
    if not source.is_file():
        raise AssertionError(f"missing fixture: {source}")
    content = source.read_bytes()
    if not content:
        raise AssertionError(f"empty fixture: {source}")
    actual = git_blob_sha(content)
    if actual != expected_sha:
        raise AssertionError(
            f"stale/tampered fixture {source}: Git blob {actual}, expected {expected_sha}"
        )


def verify_rejection(relative, result, expected_diagnostic):
    if result.returncode != 1:
        raise AssertionError(
            f"{relative}: expected compiler's specific rejection status 1; "
            f"got {result.returncode}"
        )
    if result.stdout != b"":
        raise AssertionError(f"{relative}: compiler emitted output on rejection")
    expected = (expected_diagnostic + "\n").encode("utf-8")
    if result.stderr != expected:
        raise AssertionError(
            f"{relative}: unexpected diagnostic; actual={result.stderr!r}; "
            f"expected={expected!r}. Rebaseline only with a reviewed compiler change."
        )


def verify_positive_control(compiler, clang):
    # Detect a compiler stub or a CLI that simply rejects everything.
    # This program uses only Oreslang's currently executable scalar subset.
    with tempfile.TemporaryDirectory(prefix="ores-queue-positive-") as directory:
        src = Path(directory) / "native-control.ores"
        exe = Path(directory) / "native-control"
        src.write_text("pub fnc main(): int {\n  return 42;\n}\n", encoding="utf-8")
        compiled = subprocess.run(
            [str(compiler), str(src)], capture_output=True, timeout=30, check=False
        )
        if compiled.returncode != 0 or compiled.stderr:
            raise AssertionError(
                f"supported Oreslang control failed: rc={compiled.returncode}, "
                f"stderr={compiled.stderr!r}"
            )
        if not compiled.stdout.startswith(b"; ModuleID") or \
                b"define i32 @main()" not in compiled.stdout or \
                b"ret i64 42" not in compiled.stdout:
            raise AssertionError("supported control did not emit expected verified LLVM IR")
        linked = subprocess.run(
            [str(clang), "-x", "ir", "-", "-o", str(exe)],
            input=compiled.stdout, capture_output=True, timeout=40, check=False
        )
        if linked.returncode != 0:
            raise AssertionError(
                f"Clang rejected compiler-generated IR: {linked.stderr!r}"
            )
        guest = subprocess.run([str(exe)], capture_output=True, timeout=15, check=False)
        if guest.returncode != 42:
            raise AssertionError(
                f"native execution returned {guest.returncode}, expected 42; "
                f"stderr={guest.stderr!r}"
            )
    print("PASS: supported Oreslang compiled and ran as native LLVM (exit 42)",
          flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--fixture-root", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    clang = args.clang.resolve()
    if not compiler.is_file() or not clang.is_file():
        raise AssertionError("compiler or clang executable missing")
    for relative, (sha, _) in FIXTURES.items():
        verify_fixture(args.fixture_root / relative, sha)
    verify_positive_control(compiler, clang)
    for relative, (_, diagnostic) in FIXTURES.items():
        result = subprocess.run(
            [str(compiler), str((args.fixture_root / relative).resolve())],
            capture_output=True, timeout=30, check=False,
        )
        verify_rejection(relative, result, diagnostic)
        print(f"PASS: specific parser boundary rejects {relative} without IR",
              flush=True)
    print("PASS: six Git-pinned queue fixtures fail closed (not native parity)",
          flush=True)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as exc:
        print("native linked-queue admission gate failed:", exc, file=sys.stderr)
        sys.exit(1)
