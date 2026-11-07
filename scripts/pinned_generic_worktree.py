"""Attest the actual bytes of an exact Git tree, including ignored files.

Unlike git status, Git blob hashing detects index assume-unchanged and
skip-worktree modifications. The caller independently pins the full tree SHA.
"""
import hashlib
import os
from pathlib import Path
import stat
import subprocess


def attest_pinned_worktree(git_top: Path, source_root: Path, tree_sha: str) -> None:
    git_top = git_top.resolve(strict=True)
    source_root = source_root.resolve(strict=True)
    if not source_root.is_relative_to(git_top):
        raise ValueError("canonical source is outside Git checkout")
    if not isinstance(tree_sha, str) or len(tree_sha) != 40 or any(c not in "0123456789abcdef" for c in tree_sha):
        raise ValueError("invalid canonical Git tree SHA")
    listing = subprocess.run(
        ["git", "-C", str(git_top), "ls-tree", "-r", "-z", tree_sha],
        capture_output=True, timeout=20, check=False,
    )
    if listing.returncode != 0:
        raise ValueError("cannot enumerate pinned common-source Git tree")
    expected = {}
    expected_dirs = {""}
    for record in listing.stdout.split(b"\0"):
        if not record:
            continue
        try:
            metadata, rel_bytes = record.split(b"\t", 1)
            mode, kind, blob_sha = metadata.decode("ascii").split(" ")
            rel = rel_bytes.decode("utf-8")
        except (ValueError, UnicodeDecodeError) as exc:
            raise ValueError("malformed pinned Git tree entry") from exc
        parts = rel.split("/")
        if (kind != "blob" or mode not in ("100644", "100755")
                or rel in expected or any(p in ("", ".", "..") for p in parts)):
            raise ValueError("unsafe or duplicate pinned Git tree path")
        expected[rel] = (mode, blob_sha)
        for i in range(1, len(parts)):
            expected_dirs.add("/".join(parts[:i]))
    if not expected:
        raise ValueError("empty pinned common-source Git tree")
    for current, dirs, filenames in os.walk(source_root, topdown=True, followlinks=False):
        rel_path = Path(current).relative_to(source_root)
        prefix = "" if rel_path == Path(".") else rel_path.as_posix()
        if prefix not in expected_dirs:
            raise ValueError(f"unexpected source directory: {prefix}")
        for name in list(dirs):
            relative = f"{prefix}/{name}" if prefix else name
            candidate = Path(current) / name
            if not prefix and name == ".git" and source_root == git_top:
                dirs.remove(name)
            elif candidate.is_symlink() or relative not in expected_dirs:
                raise ValueError(f"unexpected or symlinked source directory: {relative}")
        for name in filenames:
            relative = f"{prefix}/{name}" if prefix else name
            candidate = Path(current) / name
            if not prefix and name == ".git" and source_root == git_top:
                continue
            entry = expected.pop(relative, None)
            if entry is None:
                raise ValueError(f"extra source file (including ignored): {relative}")
            if candidate.is_symlink():
                raise ValueError(f"symlinked source file: {relative}")
            info = candidate.stat(follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                raise ValueError(f"nonregular source file: {relative}")
            mode, expected_sha = entry
            if bool(info.st_mode & 0o111) != (mode == "100755"):
                raise ValueError(f"modified source executable mode: {relative}")
            if info.st_size > 8 * 1024 * 1024:
                raise ValueError(f"oversized pinned source file: {relative}")
            data = candidate.read_bytes()
            blob_sha = hashlib.sha1(b"blob " + str(len(data)).encode("ascii")
                                    + b"\0" + data).hexdigest()
            if blob_sha != expected_sha:
                raise ValueError(f"source bytes differ from pinned Git tree: {relative}")
    if expected:
        raise ValueError(f"missing pinned source files: {sorted(expected)[:3]}")
