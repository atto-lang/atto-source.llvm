#!/usr/bin/env python3
"""Fail-closed Git worktree identity fence for source-local native NIO tests.

This is a worktree admission/consistency check, not a compiler test or an
attestation. Run it before and after compilation from the repository root.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys

_SHA = re.compile(r"[0-9a-f]{40}\Z", re.ASCII)
_CMAKE_DIR = re.compile(rb"cmake-build-[^/]+/")
_GIT_OVERRIDES = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                  "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT")


def git(*args: str) -> bytes:
    return subprocess.check_output(("git", "-c", "core.fileMode=true",
                                    "-c", "core.fsmonitor=false", *args),
                                   stderr=subprocess.PIPE)


def verify(commit: str, tree: str) -> None:
    if not _SHA.fullmatch(commit) or not _SHA.fullmatch(tree):
        raise ValueError("expected commit/tree must be canonical SHA-1 identifiers")
    present = [key for key in _GIT_OVERRIDES if key in os.environ]
    if present:
        raise ValueError("Git checkout overrides are forbidden: " + ", ".join(present))
    top = Path(os.fsdecode(git("rev-parse", "--show-toplevel").strip())).resolve()
    if top != Path.cwd().resolve():
        raise ValueError("qualification must run from the Git worktree root")
    observed_commit = git("rev-parse", "--verify", "HEAD^{commit}").decode().strip()
    observed_tree = git("rev-parse", "--verify", "HEAD^{tree}").decode().strip()
    if (observed_commit, observed_tree) != (commit, tree):
        raise ValueError("source commit/tree changed during qualification")

    # Both index-level assume-unchanged and skip-worktree can conceal source
    # changes from porcelain status; Git prints 'H ' for ordinary tracked paths.
    for item in git("ls-files", "-v", "-z").split(b"\0"):
        if item and not item.startswith(b"H "):
            raise ValueError("hidden/sparse index flag: " + repr(item[:120]))

    for name in ("build", ".cache"):
        if Path(name).is_symlink():
            raise ValueError("build/cache path may not be a symlink: " + name)
    for candidate in Path.cwd().glob("cmake-build-*"):
        if candidate.is_symlink():
            raise ValueError("cmake-build path may not be a symlink")

    # Explicitly override a locally disabled core.fileMode and cached fsmonitor.
    if git("status", "--porcelain=v1", "-z", "--untracked-files=all"):
        raise ValueError("dirty, untracked, or file-mode-modified source")

    # Ignored files can still affect a native build. Permit only root-scoped
    # cache/build directories; never ignored files inside source or tests.
    ignored = git("ls-files", "--others", "--ignored", "--exclude-standard", "-z")
    for name in ignored.split(b"\0"):
        if not name:
            continue
        allowed = (name.startswith((b"build/", b".cache/")) or
                   _CMAKE_DIR.match(name) is not None)
        if not allowed:
            raise ValueError("ignored non-build input: " + repr(name[:120]))



def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: verify-native-nio-source-local.py SHA1_COMMIT SHA1_TREE", file=sys.stderr)
        return 64
    try:
        verify(argv[1], argv[2])
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print("ERROR: native NIO exact-worktree gate: " + str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
