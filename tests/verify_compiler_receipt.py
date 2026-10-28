#!/usr/bin/env python3
"""Fail-closed source-local LLVM compiler receipt gate.

A receipt captures exact bytes on one trusted runner after its build step;
standalone JSON cannot authenticate an independent build or GitHub execution.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

SHA1 = re.compile(r"[0-9a-f]{40}\\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\\Z")
SCHEMA = "oreslang.llvm.compiler-receipt.v1"


class GateFailure(ValueError):
    pass


def git(root: Path, *args: str) -> str:
    p = subprocess.run(["git", "-C", str(root), *args],
                       capture_output=True, text=True, timeout=15,
                       env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                            "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0"})
    if p.returncode != 0:
        raise GateFailure("Git source verification failed")
    return p.stdout.strip()


def source_identity(root: Path, expected_head: str) -> tuple[str, str]:
    if not SHA1.fullmatch(expected_head):
        raise GateFailure("expected Git head must be a complete SHA-1")
    if root.is_symlink() or not root.is_dir() or root.resolve() != root.absolute():
        raise GateFailure("source root must be an actual absolute directory")
    if git(root, "rev-parse", "--show-toplevel") != str(root):
        raise GateFailure("source root is not the exact Git checkout root")
    head = git(root, "rev-parse", "HEAD")
    tree = git(root, "rev-parse", "HEAD^{tree}")
    if head != expected_head or not SHA1.fullmatch(tree) or \\
            tree != git(root, "show", "-s", "--format=%T", "HEAD"):
        raise GateFailure("Git head or tree identity mismatch")
    if git(root, "status", "--porcelain", "--untracked-files=no"):
        raise GateFailure("tracked source checkout is dirty")
    return head, tree


def compiler_digest(root: Path, binary: Path) -> str:
    if binary.is_symlink() or binary.resolve() != root / "build" / "oreslang-llvmc":
        raise GateFailure("compiler must be an in-tree, non-symlink build/oreslang-llvmc")
    try:
        # O_NOFOLLOW prevents a last-component symlink substitution at open.
        fd = os.open(binary, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as src:
            metadata = os.fstat(src.fileno())
            if not stat.S_ISREG(metadata.st_mode) or not (metadata.st_mode & 0o111):
                raise GateFailure("compiler is not a regular executable")
            if metadata.st_size < 1 or metadata.st_size > 256 * 1024 * 1024:
                raise GateFailure("compiler size outside 256 MiB gate")
            sha = hashlib.sha256()
            for chunk in iter(lambda: src.read(1024 * 1024), b""):
                sha.update(chunk)
            if src.tell() != metadata.st_size:
                raise GateFailure("compiler binary changed while hashing")
            return sha.hexdigest()
    except OSError as exc:
        raise GateFailure("compiler executable unavailable or unsafe") from exc


def validate_receipt(raw: object, *, head: str, tree: str, digest: str) -> None:
    if not isinstance(raw, dict) or set(raw) != {"schema", "sourceCommit", "sourceTree", "compilerSha256"}:
        raise GateFailure("receipt has unexpected or missing fields")
    if raw["schema"] != SCHEMA or raw["sourceCommit"] != head or \\
            raw["sourceTree"] != tree or raw["compilerSha256"] != digest:
        raise GateFailure("receipt does not match this source and compiler executable")
    if not SHA256.fullmatch(raw["compilerSha256"]):
        raise GateFailure("invalid compiler digest")


def run(mode: str, source: str, compiler: str, expected_head: str, receipt: str) -> None:
    root, binary, output = Path(source), Path(compiler), Path(receipt)
    if not all(x.is_absolute() for x in (root, binary, output)):
        raise GateFailure("source, compiler and receipt paths must be absolute")
    head, tree = source_identity(root, expected_head)
    digest = compiler_digest(root, binary)
    data = {"schema": SCHEMA, "sourceCommit": head,
            "sourceTree": tree, "compilerSha256": digest}
    if mode == "capture":
        # Prevent accidentally trusting a receipt left over from a prior run.
        old_umask = os.umask(0o077)
        try:
            with output.open("x", encoding="utf-8") as f:
                json.dump(data, f, sort_keys=True)
                f.write("\\n")
        finally:
            os.umask(old_umask)
        print("PINNED_LLVM_COMPILER_CAPTURED", head, tree, digest)
    elif mode == "verify":
        if output.is_symlink() or output.stat().st_size > 8192:
            raise GateFailure("unsafe compiler receipt")
        try:
            with output.open("r", encoding="utf-8") as f:
                raw = json.load(f)
        except (OSError, ValueError) as exc:
            raise GateFailure("compiler receipt is missing or invalid") from exc
        validate_receipt(raw, head=head, tree=tree, digest=digest)
        print("PINNED_LLVM_COMPILER_VERIFIED", head, tree, digest)
    else:
        raise GateFailure("unknown receipt mode")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("capture", "verify"))
    for name in ("source", "compiler", "expected-head", "receipt"):
        parser.add_argument("--" + name, required=True)
    a = parser.parse_args()
    try:
        run(a.mode, a.source, a.compiler, a.expected_head, a.receipt)
        return 0
    except (GateFailure, FileExistsError, OSError) as exc:
        print("NOT_EXECUTED: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
