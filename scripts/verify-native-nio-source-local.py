#!/usr/bin/env python3
"""Fail-closed Git worktree identity fence for source-local native NIO tests.

This is a worktree admission/consistency check, not a compiler test or an
attestation. Run it before and after compilation from the repository root.
"""
from __future__ import annotations

import os
import hashlib
import stat
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


def verify_raw_tracked_blobs() -> None:
    """Hash actual compiler-visible bytes, bypassing Git clean/smudge filters.

    `git status` can claim clean for a modified source when an untracked local
    attributes rule enables a normalizing `filter.*.clean`. In that case the
    committed/index blob and the actual compiler input are different. Compare
    each file's raw contents directly against the staged Git SHA-1 object.
    Index entries are NUL-delimited to handle arbitrary path bytes.
    """
    for entry in git("ls-files", "--stage", "-z").split(b"\0"):
        if not entry:
            continue
        metadata, sep, path = entry.partition(b"\t")
        if not sep or not path:
            raise ValueError("invalid tracked index entry")
        fields = metadata.split(b" ")
        if len(fields) != 3 or fields[2] != b"0":
            raise ValueError("unmerged or invalid tracked index entry")
        mode, object_id, _ = fields
        if mode in (b"100644", b"100755"):
            # O_NOFOLLOW ensures a source-file symlink substitution cannot
            # redirect verification outside this checkout.
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            try:
                before = os.fstat(fd)
                if not stat.S_ISREG(before.st_mode):
                    raise ValueError("tracked source is not a regular file: " + repr(path[:120]))
                # A verified inode with another hardlink can be modified
                # through a path outside the checkout after verification.
                # Source-local compilation must not trust such an alias.
                if before.st_nlink != 1:
                    raise ValueError("multiply-linked tracked source: " + repr(path[:120]))
                if bool(before.st_mode & 0o111) != (mode == b"100755"):
                    raise ValueError("tracked source executable mode mismatch: " + repr(path[:120]))
                digest = hashlib.sha1()
                digest.update(b"blob " + str(before.st_size).encode("ascii") + b"\0")
                while True:
                    chunk = os.read(fd, 1024 * 1024)
                    if not chunk:
                        break
                    digest.update(chunk)
                after = os.fstat(fd)
                if (before.st_size, before.st_mtime_ns, before.st_ctime_ns,
                        before.st_ino, before.st_dev, before.st_nlink) != (
                        after.st_size, after.st_mtime_ns, after.st_ctime_ns,
                        after.st_ino, after.st_dev, after.st_nlink):
                    raise ValueError("tracked source changed during verification: " + repr(path[:120]))
                if digest.hexdigest().encode("ascii") != object_id:
                    raise ValueError("raw tracked blob mismatch: " + repr(path[:120]))
            finally:
                os.close(fd)
        elif mode == b"120000":
            if not stat.S_ISLNK(os.lstat(path).st_mode):
                raise ValueError("tracked symlink type mismatch: " + repr(path[:120]))
            raw_target = os.readlink(path)
            digest = hashlib.sha1(b"blob " + str(len(raw_target)).encode("ascii")
                                  + b"\0" + raw_target).hexdigest().encode("ascii")
            if digest != object_id:
                raise ValueError("raw tracked symlink mismatch: " + repr(path[:120]))
        else:
            raise ValueError("unsupported tracked file mode: " + repr(mode))


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

    # Git status can execute attacker-configured clean-filter commands while
    # checking the worktree. Compare HEAD to the staged index directly (without
    # working-file conversion, textconv, or external diff), then hash each raw
    # tracked worktree file independently; never invoke a clean/smudge filter.
    diff = subprocess.run(("git", "-c", "core.fileMode=true",
                           "-c", "core.fsmonitor=false", "diff", "--cached",
                           "--quiet", "--no-ext-diff", "--no-textconv",
                           "HEAD", "--"), capture_output=True, check=False)
    if diff.returncode != 0:
        raise ValueError("staged Git index differs from HEAD")
    verify_raw_tracked_blobs()
    if git("ls-files", "--others", "--exclude-standard", "-z"):
        raise ValueError("dirty or untracked source")

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
